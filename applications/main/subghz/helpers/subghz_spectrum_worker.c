#include "subghz_spectrum_worker.h"
#include "subghz_rssi_scan.h"
#include <lib/drivers/cc1101.h>
#include <furi_hal_aw9523.h>
#include <furi_hal_power.h>
#include <furi_hal_subghz.h>

#include <furi.h>
#include <esp_heap_caps.h>

/* Profiling: set to 1 to log a per-sweep timing breakdown — tune (SPI) vs
 * measure (settle busy-wait + RSSI reads) per bin — over serial at LOG_I.
 * Zero cost when 0. Flip to 1 and reflash to MEASURE before changing any RF
 * timing (settle/bins/samples): it tells you whether a sweep is tune-SPI-bound
 * or settle-bound. See subghz_spectrum_worker_thread(). */
#ifndef SUBGHZ_SPECTRUM_PROFILE
#define SUBGHZ_SPECTRUM_PROFILE 0
#endif
#if SUBGHZ_SPECTRUM_PROFILE
#include <esp_timer.h>
#endif

#define TAG "SubGhzSpectrumWorker"

/* Per-bin RSSI settle scales with the RX filter bandwidth, exactly like the
 * frequency analyzer (subghz_rssi_scan_settle_us): the narrow filters used at
 * small spans decimate harder, so their AGC needs ~1 ms, while wide filters
 * (big spans) settle in the 300 us floor. The old flat 1 ms was needlessly slow
 * for wide spans; a flat 600 us was too short for the 58 kHz filter and read
 * RSSI before the AGC settled (weak/noisy bins). Scaling gives accurate narrow
 * sweeps AND faster/smoother wide sweeps. Same scale constant as the analyzer. */
#define SUBGHZ_SPECTRUM_SETTLE_SCALE 60000000UL

/* A sweep in which not a single bin could be measured (SPI error, CC1101 not
 * reaching RX, radio reset by a supply dip) used to be indistinguishable from
 * "no signal": every bin silently became -127 dBm. After this many such sweeps
 * in a row the radio is reset and reconfigured, with a pause so a dead radio
 * does not turn the worker into a busy loop. */
#define SUBGHZ_SPECTRUM_DEAD_SWEEPS_RECOVER 3
#define SUBGHZ_SPECTRUM_RECOVER_PAUSE_MS    250
/* Log the first dead sweep, then only every Nth one, so a persistent fault
 * cannot flood the console. */
#define SUBGHZ_SPECTRUM_DEAD_LOG_EVERY 20
/* Summary line (sweep rate + strongest bin) at LOG_I this often, like the
 * Frequency Analyzer's "scan:" line, so a running spectrum is visible on serial. */
#define SUBGHZ_SPECTRUM_STATS_MS 5000

#define SUBGHZ_SPECTRUM_DEFAULT_CENTER 433920000UL
#define SUBGHZ_SPECTRUM_DEFAULT_SPAN 6000000UL

/* CC1101 channel-filter bandwidths and their MDMCFG4 upper nibble
 * ((CHANBW_E << 2) | CHANBW_M). We keep the lower nibble (DRATE_E) at 0b0111,
 * matching the frequency analyzer presets. Sorted ascending by bandwidth so we
 * can pick the smallest filter that still covers a bin's width (BW >= step). */
typedef struct {
    uint32_t bw;
    uint8_t nibble;
} SubGhzSpectrumBw;

static const SubGhzSpectrumBw subghz_spectrum_bw_table[] = {
    {58000, 0xF},
    {68000, 0xE},
    {81000, 0xD},
    {102000, 0xC},
    {116000, 0xB},
    {135000, 0xA},
    {162000, 0x9},
    {203000, 0x8},
    {232000, 0x7},
    {270000, 0x6},
    {325000, 0x5},
    {406000, 0x4},
    {464000, 0x3},
    {541000, 0x2},
    {650000, 0x1},
    {812000, 0x0},
};

/* The AUTO scanner hops through 6 bands; keep a couple of spare windows so a
 * manual centre/span change does not evict a band that is about to be revisited. */
#define SUBGHZ_SPECTRUM_CAL_SLOTS 8

/* FSCAL values drift with temperature/supply. The old per-hop recalibration in
 * AUTO mode hid that; a cached window is recalibrated after this long instead. */
#define SUBGHZ_SPECTRUM_CAL_MAX_AGE_MS 60000UL

typedef struct {
    bool used;
    uint32_t center;
    uint32_t span;
    uint32_t born_tick; // when the points were (re)started, for the age limit
    SubGhzRssiScanPoint points[SUBGHZ_SPECTRUM_BINS];
} SubGhzSpectrumCalSlot;

struct SubGhzSpectrumWorker {
    FuriThread* thread;
    volatile bool worker_running;
    FuriMutex* mutex;

    uint32_t center;
    uint32_t span;
    uint32_t window_generation;

    SubGhzSpectrumWorkerSweepCallback callback;
    void* context;

    float rssi[SUBGHZ_SPECTRUM_BINS];
    SubGhzSpectrumCalSlot* cal_slots; // SUBGHZ_SPECTRUM_CAL_SLOTS windows, PSRAM-first
    uint8_t cal_next; // round-robin victim when no slot matches
};

/** Calibration cache for the window (center, span) being swept, created on first
 * use. Keyed by window rather than wiped on every window change: the AUTO band
 * scanner changes the window after every sweep, so a single cache that is reset
 * per change made it recalibrate all bins (SCAL + SPI round trips) on every hop. */
static SubGhzRssiScanPoint* subghz_spectrum_worker_cal_get(
    SubGhzSpectrumWorker* instance,
    uint32_t center,
    uint32_t span) {
    const uint32_t now = furi_get_tick();
    const uint32_t max_age = furi_ms_to_ticks(SUBGHZ_SPECTRUM_CAL_MAX_AGE_MS);
    for(size_t i = 0; i < SUBGHZ_SPECTRUM_CAL_SLOTS; i++) {
        SubGhzSpectrumCalSlot* slot = &instance->cal_slots[i];
        if(slot->used && slot->center == center && slot->span == span) {
            if((now - slot->born_tick) > max_age) {
                memset(slot->points, 0, sizeof(slot->points));
                slot->born_tick = now;
            }
            return slot->points;
        }
    }
    SubGhzSpectrumCalSlot* slot = &instance->cal_slots[instance->cal_next];
    instance->cal_next = (instance->cal_next + 1) % SUBGHZ_SPECTRUM_CAL_SLOTS;
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->center = center;
    slot->span = span;
    slot->born_tick = now;
    return slot->points;
}

static void subghz_spectrum_worker_cal_clear(SubGhzSpectrumWorker* instance) {
    memset(instance->cal_slots, 0, sizeof(SubGhzSpectrumCalSlot) * SUBGHZ_SPECTRUM_CAL_SLOTS);
    instance->cal_next = 0;
}

/** Set the RF band switch for boards with an external RF switch (T-Embed). */
static void subghz_spectrum_worker_set_path(uint32_t freq) {
    if(freq >= 281000000 && freq <= 361000000) {
        furi_hal_subghz_set_path(FuriHalSubGhzPath315);
    } else if(freq >= 378000000 && freq <= 481000000) {
        furi_hal_subghz_set_path(FuriHalSubGhzPath433);
    } else if(freq >= 749000000 && freq <= 962000000) {
        furi_hal_subghz_set_path(FuriHalSubGhzPath868);
    } else {
        furi_hal_subghz_set_path(FuriHalSubGhzPathIsolate);
    }
}

/** Pick the smallest CC1101 RX filter whose bandwidth still covers `step`,
 * returning both its MDMCFG4 nibble and its bandwidth in Hz (so the caller can
 * scale the AGC settle time to the filter, like the frequency analyzer does). */
static uint8_t subghz_spectrum_worker_bw_pick(uint32_t step, uint32_t* bw_hz) {
    for(size_t i = 0; i < COUNT_OF(subghz_spectrum_bw_table); i++) {
        if(subghz_spectrum_bw_table[i].bw >= step) {
            if(bw_hz) *bw_hz = subghz_spectrum_bw_table[i].bw;
            return subghz_spectrum_bw_table[i].nibble;
        }
    }
    if(bw_hz) *bw_hz = subghz_spectrum_bw_table[COUNT_OF(subghz_spectrum_bw_table) - 1].bw;
    return 0x0;
}

static bool subghz_spectrum_worker_window_is_current(
    SubGhzSpectrumWorker* instance,
    uint32_t generation) {
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    bool is_current = instance->window_generation == generation;
    furi_mutex_release(instance->mutex);
    return is_current;
}

static void subghz_spectrum_worker_setup_registers(void) {
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    cc1101_flush_rx(&furi_hal_spi_bus_handle_subghz);
    cc1101_flush_tx(&furi_hal_spi_bus_handle_subghz);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_IOCFG0, CC1101IocfgHW);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_MDMCFG3, 0b01111111); // symbol rate
    // AGC config identical to the frequency analyzer; AGCCTRL0 selects 64 samples.
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_AGCCTRL2, 0b00000111);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_AGCCTRL1, 0b00001000);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_AGCCTRL0, 0b00110000);
    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);
}

static int32_t subghz_spectrum_worker_thread(void* context) {
    SubGhzSpectrumWorker* instance = context;
    /* Pin the CPU at full speed for the whole sweep (same as the frequency
     * analyzer worker): each of the ~256+ SPI transactions per sweep would
     * otherwise take and drop the DFS max-frequency lock, flipping the CPU
     * 80<->160 MHz continuously. Light sleep is already inhibited by the view. */
    furi_hal_power_insomnia_enter();

    furi_hal_subghz_reset();
    subghz_spectrum_worker_setup_registers();
    furi_hal_subghz_set_path(FuriHalSubGhzPathIsolate);

    uint32_t dead_sweeps = 0; // consecutive sweeps since the last reset attempt
    uint32_t dead_total = 0; // consecutive sweeps without a single measured bin
    uint32_t stats_at = furi_get_tick();
    uint32_t stats_sweeps = 0;
    uint32_t stats_measured = 0;
    float stats_peak_rssi = -127.0f;
    uint32_t stats_peak_freq = 0;

    while(instance->worker_running) {
        // Snapshot the requested window (the view may change it any time).
        furi_mutex_acquire(instance->mutex, FuriWaitForever);
        uint32_t center = instance->center;
        uint32_t span = instance->span;
        uint32_t generation = instance->window_generation;
        furi_mutex_release(instance->mutex);

        uint32_t step = span / SUBGHZ_SPECTRUM_BINS;
        if(step == 0) step = 1;
        int64_t f_start_signed = (int64_t)center - (int64_t)(span / 2);
        if(f_start_signed < 0) f_start_signed = 0;
        uint32_t f_start = (uint32_t)f_start_signed;
        SubGhzRssiScanPoint* calibration_points =
            subghz_spectrum_worker_cal_get(instance, center, span);

        // Configure the bin-width filter for this sweep and scale the per-bin
        // settle to that filter's bandwidth (narrow filter -> longer AGC settle).
        uint32_t bw_hz = 0;
        const uint8_t bw_nibble = subghz_spectrum_worker_bw_pick(step, &bw_hz);
        const uint32_t settle_us =
            subghz_rssi_scan_settle_us(bw_hz, SUBGHZ_SPECTRUM_SETTLE_SCALE);
        furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
        cc1101_switch_to_idle(&furi_hal_spi_bus_handle_subghz);
        cc1101_write_reg(
            &furi_hal_spi_bus_handle_subghz, CC1101_MDMCFG4, (uint8_t)((bw_nibble << 4) | 0b0111));
        furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);

        const bool rx_path_ready = furi_hal_aw9523_set_rx();
        uint32_t sweep_attempted = 0; // bins inside a valid band
        uint32_t sweep_measured = 0; // bins that really produced an RSSI reading
#if SUBGHZ_SPECTRUM_PROFILE
        uint64_t prof_tune_us = 0, prof_meas_us = 0;
        uint32_t prof_tune_max = 0, prof_meas_max = 0, prof_bins = 0, prof_fails = 0;
#endif
        for(size_t i = 0;
            i < SUBGHZ_SPECTRUM_BINS && instance->worker_running &&
            subghz_spectrum_worker_window_is_current(instance, generation);
            i++) {
            const uint32_t freq = f_start + (uint32_t)(step * i) + step / 2;
            if(!furi_hal_subghz_is_frequency_valid(freq)) {
                instance->rssi[i] = -127.0f; // legitimately outside the CC1101 bands
                continue;
            }
            sweep_attempted++;
            if(!rx_path_ready) {
                instance->rssi[i] = -127.0f;
                continue;
            }

            subghz_spectrum_worker_set_path(freq);

#if SUBGHZ_SPECTRUM_PROFILE
            const int64_t prof_t0 = esp_timer_get_time();
#endif
            SubGhzRssiScanPoint* point = &calibration_points[i];
            if(!point->valid) {
                if(!subghz_rssi_scan_calibrate(freq, point)) {
                    instance->rssi[i] = -127.0f;
#if SUBGHZ_SPECTRUM_PROFILE
                    prof_fails++;
#endif
                    continue;
                }
            }
            const bool tuned = subghz_rssi_scan_tune(point);
#if SUBGHZ_SPECTRUM_PROFILE
            const int64_t prof_t1 = esp_timer_get_time();
#endif
            if(!tuned) {
                instance->rssi[i] = -127.0f;
#if SUBGHZ_SPECTRUM_PROFILE
                prof_fails++;
#endif
                continue;
            }

            furi_delay_us(settle_us);
            instance->rssi[i] = furi_hal_subghz_get_rssi();
            sweep_measured++;
            if(instance->rssi[i] > stats_peak_rssi) {
                stats_peak_rssi = instance->rssi[i];
                stats_peak_freq = freq;
            }
#if SUBGHZ_SPECTRUM_PROFILE
            const uint32_t prof_tune = (uint32_t)(prof_t1 - prof_t0);
            const uint32_t prof_meas = (uint32_t)(esp_timer_get_time() - prof_t1);
            prof_tune_us += prof_tune;
            prof_meas_us += prof_meas;
            if(prof_tune > prof_tune_max) prof_tune_max = prof_tune;
            if(prof_meas > prof_meas_max) prof_meas_max = prof_meas;
            prof_bins++;
#endif
        }

        const bool sweep_complete = instance->worker_running &&
                                    subghz_spectrum_worker_window_is_current(instance, generation);
        if(sweep_complete && instance->callback) {
            instance->callback(instance->context, instance->rssi, center, span);
        }

        if(sweep_complete) {
            stats_sweeps++;
            stats_measured += sweep_measured;
        }
        const uint32_t now = furi_get_tick();
        if(now - stats_at >= SUBGHZ_SPECTRUM_STATS_MS) {
            FURI_LOG_I(
                TAG,
                "sweep: %lu sweeps/%lu ms (%lu ms each), center %lu span %lu, "
                "%lu bins measured, peak %d dBm @ %lu Hz",
                (unsigned long)stats_sweeps,
                (unsigned long)(now - stats_at),
                (unsigned long)(stats_sweeps ? (now - stats_at) / stats_sweeps : 0),
                (unsigned long)center,
                (unsigned long)span,
                (unsigned long)stats_measured,
                (int)stats_peak_rssi,
                (unsigned long)stats_peak_freq);
            stats_at = now;
            stats_sweeps = 0;
            stats_measured = 0;
            stats_peak_rssi = -127.0f;
            stats_peak_freq = 0;
        }

        // A sweep cut short by a window change or by stop says nothing about the radio.
        if(sweep_complete && sweep_attempted > 0) {
            if(sweep_measured > 0) {
                if(dead_total) {
                    FURI_LOG_I(
                        TAG, "Radio is measuring again after %lu dead sweeps", (unsigned long)dead_total);
                }
                dead_sweeps = 0;
                dead_total = 0;
            } else {
                dead_sweeps++;
                dead_total++;
                if(dead_total == 1 || dead_total % SUBGHZ_SPECTRUM_DEAD_LOG_EVERY == 0) {
                    FURI_LOG_W(
                        TAG,
                        "Sweep measured 0/%lu bins (rx_path=%d, dead sweeps=%lu): no RX state or SPI error",
                        (unsigned long)sweep_attempted,
                        (int)rx_path_ready,
                        (unsigned long)dead_total);
                }
                if(dead_sweeps >= SUBGHZ_SPECTRUM_DEAD_SWEEPS_RECOVER) {
                    // The chip may have reset (supply dip) or got stuck; every cached
                    // calibration would then be stale as well, so drop them too.
                    furi_hal_subghz_reset();
                    subghz_spectrum_worker_setup_registers();
                    furi_hal_subghz_set_path(FuriHalSubGhzPathIsolate);
                    subghz_spectrum_worker_cal_clear(instance);
                    dead_sweeps = 0;
                    furi_delay_ms(SUBGHZ_SPECTRUM_RECOVER_PAUSE_MS);
                }
            }
        }

#if SUBGHZ_SPECTRUM_PROFILE
        if(prof_bins) {
            FURI_LOG_I(
                TAG,
                "sweep: bins=%lu tune avg/max=%lu/%lu us meas avg/max=%lu/%lu us settle=%lu fails=%lu",
                (unsigned long)prof_bins,
                (unsigned long)(prof_tune_us / prof_bins),
                (unsigned long)prof_tune_max,
                (unsigned long)(prof_meas_us / prof_bins),
                (unsigned long)prof_meas_max,
                (unsigned long)settle_us,
                (unsigned long)prof_fails);
        }
#endif

        furi_delay_ms(1); // let the GUI thread run
    }

    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();
    furi_hal_power_insomnia_exit();
    return 0;
}

SubGhzSpectrumWorker* subghz_spectrum_worker_alloc(void) {
    SubGhzSpectrumWorker* instance = calloc(1, sizeof(SubGhzSpectrumWorker));
    if(!instance) return NULL;
    // ~24 KB of calibration windows: the CPU is the only reader, so PSRAM only.
    // No fallback to the default heap: that would take 24 KB of the scarce
    // internal DRAM, and a failed start is reported to the user gracefully.
    const size_t cal_bytes = sizeof(SubGhzSpectrumCalSlot) * SUBGHZ_SPECTRUM_CAL_SLOTS;
    instance->cal_slots = heap_caps_calloc(1, cal_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!instance->cal_slots) {
        free(instance);
        return NULL;
    }
    instance->thread =
        furi_thread_alloc_ex("SubGhzSpectrumWorker", 4096, subghz_spectrum_worker_thread, instance);
    instance->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    // The sweep busy-waits for RSSI settling; keep rendering and input ahead of it.
    if(instance->thread) furi_thread_set_priority(instance->thread, FuriThreadPriorityLow);
    if(!instance->thread || !instance->mutex) {
        if(instance->thread) furi_thread_free(instance->thread);
        if(instance->mutex) furi_mutex_free(instance->mutex);
        free(instance->cal_slots);
        free(instance);
        return NULL;
    }
    instance->worker_running = false;
    instance->center = SUBGHZ_SPECTRUM_DEFAULT_CENTER;
    instance->span = SUBGHZ_SPECTRUM_DEFAULT_SPAN;
    instance->callback = NULL;
    instance->context = NULL;
    return instance;
}

void subghz_spectrum_worker_free(SubGhzSpectrumWorker* instance) {
    if(!instance) return;
    furi_thread_free(instance->thread);
    furi_mutex_free(instance->mutex);
    free(instance->cal_slots);
    free(instance);
}

void subghz_spectrum_worker_set_callback(
    SubGhzSpectrumWorker* instance,
    SubGhzSpectrumWorkerSweepCallback callback,
    void* context) {
    furi_assert(instance);
    instance->callback = callback;
    instance->context = context;
}

void subghz_spectrum_worker_set_window(
    SubGhzSpectrumWorker* instance,
    uint32_t center,
    uint32_t span) {
    furi_assert(instance);
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->center = center;
    instance->span = span;
    instance->window_generation++;
    furi_mutex_release(instance->mutex);
}

void subghz_spectrum_worker_start(SubGhzSpectrumWorker* instance) {
    furi_assert(instance);
    furi_assert(!instance->worker_running);
    instance->worker_running = true;
    furi_thread_start(instance->thread);
}

void subghz_spectrum_worker_stop(SubGhzSpectrumWorker* instance) {
    furi_assert(instance);
    furi_assert(instance->worker_running);
    instance->worker_running = false;
    furi_thread_join(instance->thread);
}

bool subghz_spectrum_worker_is_running(SubGhzSpectrumWorker* instance) {
    furi_assert(instance);
    return instance->worker_running;
}
