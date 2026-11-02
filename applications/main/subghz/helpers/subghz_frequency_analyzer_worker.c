#include "subghz_frequency_analyzer_worker.h"
#include "subghz_rssi_scan.h"
#include <lib/drivers/cc1101.h>
#include <furi_hal_aw9523.h>
#include <furi_hal_power.h>
#include <furi_hal_subghz.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>

#include <furi.h>

/** Set RF band switch for the given frequency (needed on boards with external RF switch) */
static void subghz_fa_worker_set_path_for_frequency(uint32_t freq) {
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

#define TAG "SubghzFrequencyAnalyzerWorker"

#define SUBGHZ_FREQUENCY_ANALYZER_THRESHOLD -97.0f

/* Fine scan: +/-300 kHz around the coarse hit in 20 kHz steps. */
#define SUBGHZ_FA_FINE_HALF_SPAN 300000
#define SUBGHZ_FA_FINE_STEP      20000

/* RSSI settle = SETTLE_SCALE / filter bandwidth, clamped to 300..1200 us:
 * 300 us for the 650 kHz coarse filter, ~1 ms for the 58 kHz fine filter
 * (the analyzer keeps its 32-sample AGC wait). */
#define SUBGHZ_FA_SETTLE_SCALE 60000000UL
#define SUBGHZ_FA_BW_COARSE    650000UL
#define SUBGHZ_FA_BW_FINE      58000UL

/* Hold timing, in real time rather than scan cycles (a cycle's length depends
 * on the frequency list, the radio SPI clock and whether a fine scan ran). */
#define SUBGHZ_FA_SIGNAL_LOST_MS 300 // un-highlight the frequency
#define SUBGHZ_FA_HOLD_MS        1500 // unlock, push the frequency to history
/* Recalibrate the cached synthesizer values periodically (temperature drift). */
#define SUBGHZ_FA_RECAL_MS 30000
/* Spread each recal over several scan cycles: at most this many frequencies are
 * recalibrated per cycle. The whole-list recal (~count x ~1 ms of SPI with the
 * bus repeatedly taken) otherwise lands in one cycle and shows up as a periodic
 * hitch on the shared-bus display. */
#define SUBGHZ_FA_RECAL_BATCH 8
/* Log the scan rate this often when debug logging is on. */
#define SUBGHZ_FA_STATS_MS 5000

static const uint8_t subghz_preset_ook_58khz[][2] = {
    {CC1101_MDMCFG4, 0b11110111}, // Rx BW filter is 58.035714kHz
    /* End  */
    {0, 0},
};

static const uint8_t subghz_preset_ook_650khz[][2] = {
    {CC1101_MDMCFG4, 0b00010111}, // Rx BW filter is 650.000kHz
    /* End  */
    {0, 0},
};

struct SubGhzFrequencyAnalyzerWorker {
    FuriThread* thread;

    volatile bool worker_running;
    SubGhzSetting* setting;

    float trigger_level;

    SubGhzFrequencyAnalyzerWorkerPairCallback pair_callback;
    void* context;
};

static void subghz_frequency_analyzer_worker_load_registers(const uint8_t data[][2]) {
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    size_t i = 0;
    while(data[i][0]) {
        cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, data[i][0], data[i][1]);
        i++;
    }
    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);
}

static bool subghz_frequency_analyzer_worker_is_scannable(uint32_t frequency) {
    return furi_hal_subghz_is_frequency_valid(frequency) && frequency != 462750000 &&
           frequency != 467750000 && frequency != 464000000 && frequency <= 920000000;
}

/** Calibrate every coarse frequency once; scans then only restore the cached
 * synthesizer values instead of running a ~0.8 ms SCAL per frequency. */
static void subghz_frequency_analyzer_worker_calibrate(
    SubGhzFrequencyAnalyzerWorker* instance,
    SubGhzRssiScanPoint* points,
    size_t count) {
    const uint32_t started_at = furi_get_tick();
    size_t failed = 0;
    for(size_t i = 0; instance->worker_running && i < count; i++) {
        const uint32_t frequency = subghz_setting_get_frequency(instance->setting, i);
        points[i].valid = false;
        if(!subghz_frequency_analyzer_worker_is_scannable(frequency)) continue;
        if(!subghz_rssi_scan_calibrate(frequency, &points[i])) failed++;
    }
    if(failed) FURI_LOG_E(TAG, "CC1101 calibration failed for %u frequencies", (unsigned)failed);
    FURI_LOG_D(
        TAG,
        "Calibrated %u frequencies in %lu ms",
        (unsigned)count,
        furi_get_tick() - started_at);
}

static void subghz_frequency_analyzer_worker_publish(
    SubGhzFrequencyAnalyzerWorker* instance,
    uint32_t frequency,
    float rssi,
    bool signal) {
    if(instance->pair_callback) {
        instance->pair_callback(instance->context, frequency, rssi, signal);
    }
}

/** Worker thread
 *
 * @param context
 * @return exit code
 */
static int32_t subghz_frequency_analyzer_worker_thread(void* context) {
    SubGhzFrequencyAnalyzerWorker* instance = context;
    furi_hal_power_light_sleep_inhibit_enter();
    /* Pin the CPU at full speed for the whole scan. Every SPI transaction does
     * insomnia enter/exit, which takes/drops the DFS max-frequency lock; the
     * scan makes ~100+ transactions per cycle with a busy-wait between each, so
     * without this the CPU flips 80<->160 MHz continuously (interrupts masked
     * during each switch) and everything else, GUI included, runs at 80 MHz. */
    furi_hal_power_insomnia_enter();

    //Start CC1101
    furi_hal_subghz_reset();

    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    cc1101_flush_rx(&furi_hal_spi_bus_handle_subghz);
    cc1101_flush_tx(&furi_hal_spi_bus_handle_subghz);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_IOCFG0, CC1101IocfgHW);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_MDMCFG3,
                     0b01111111); // symbol rate
    cc1101_write_reg(
        &furi_hal_spi_bus_handle_subghz,
        CC1101_AGCCTRL2,
        0b00000111); // 00 - DVGA all; 000 - MAX LNA+LNA2; 111 - MAGN_TARGET 42 dB
    cc1101_write_reg(
        &furi_hal_spi_bus_handle_subghz,
        CC1101_AGCCTRL1,
        0b00001000); // 0; 0 - LNA 2 gain is decreased to minimum before decreasing LNA gain; 00 - Relative carrier sense threshold disabled; 1000 - Absolute carrier sense threshold disabled
    cc1101_write_reg(
        &furi_hal_spi_bus_handle_subghz,
        CC1101_AGCCTRL0,
        0b00110000); // 00 - No hysteresis, medium asymmetric dead zone, medium gain ; 11 - 64 samples agc; 00 - Normal AGC, 00 - 4dB boundary

    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);

    furi_hal_subghz_set_path(FuriHalSubGhzPathIsolate);

    const size_t count = subghz_setting_get_frequency_count(instance->setting);
    SubGhzRssiScanPoint* points = count ? calloc(count, sizeof(SubGhzRssiScanPoint)) : NULL;
    if(count && !points) {
        FURI_LOG_E(TAG, "No memory for %u calibration points", (unsigned)count);
    }

    const uint32_t settle_coarse_us =
        subghz_rssi_scan_settle_us(SUBGHZ_FA_BW_COARSE, SUBGHZ_FA_SETTLE_SCALE);
    const uint32_t settle_fine_us =
        subghz_rssi_scan_settle_us(SUBGHZ_FA_BW_FINE, SUBGHZ_FA_SETTLE_SCALE);

    subghz_frequency_analyzer_worker_load_registers(subghz_preset_ook_650khz);
    if(points) subghz_frequency_analyzer_worker_calibrate(instance, points, count);
    uint32_t calibrated_at = furi_get_tick();
    size_t recal_index = 0; // rolling cursor for incremental recalibration

    // Hold state: what is shown after the signal disappears, and since when.
    bool holding = false;
    bool lost_sent = false;
    uint32_t last_hit_at = 0;
    uint32_t hold_frequency = 0;
    float hold_rssi = 0;

    uint32_t stats_at = furi_get_tick();
    uint32_t stats_cycles = 0;
    uint32_t stats_fine = 0; // cycles that ran a fine scan
    uint32_t stats_published = 0; // callbacks into the view
    uint64_t stats_tune_us = 0; // time in coarse tune (SPI + bus waits)
    uint32_t stats_tunes = 0;
    uint64_t stats_publish_us = 0; // time inside the view callback
    uint64_t stats_cycle_us = 0; // time scanning, excluding the 5 ms pause
    /* RX entry failures are only counted here and reported with the stats line: one
     * console line per frequency per cycle floods the log (and the USB console
     * stalls the scan) exactly when the radio is already misbehaving. */
    uint32_t stats_rx_fail = 0;

    while(instance->worker_running) {
        furi_delay_ms(5); // let lower priority threads run between cycles

        if(!points) continue;

        if(furi_get_tick() - calibrated_at > SUBGHZ_FA_RECAL_MS) {
            /* Incremental recal: a few frequencies per cycle, each with its own
             * acquire/release inside subghz_rssi_scan_calibrate(), so the shared
             * SPI bus is never held for the whole burst. The timer only resets
             * once the rolling cursor wraps, so all points are still refreshed
             * within one SUBGHZ_FA_RECAL_MS window. Single-threaded worker, so a
             * point briefly marked invalid during its own recal is restored
             * before the coarse scan below runs. */
            for(size_t n = 0; n < SUBGHZ_FA_RECAL_BATCH && instance->worker_running; n++) {
                const uint32_t frequency =
                    subghz_setting_get_frequency(instance->setting, recal_index);
                if(subghz_frequency_analyzer_worker_is_scannable(frequency)) {
                    subghz_rssi_scan_calibrate(frequency, &points[recal_index]);
                }
                if(++recal_index >= count) {
                    recal_index = 0;
                    calibrated_at = furi_get_tick();
                    break;
                }
            }
        }

        if(!furi_hal_aw9523_set_rx()) continue;

        const int64_t cycle_started_us = esp_timer_get_time();

        // First stage: coarse scan over the frequency list, 650 kHz filter
        float rssi_coarse = SUBGHZ_RSSI_SCAN_RSSI_NONE;
        const SubGhzRssiScanPoint* coarse_point = NULL;
        for(size_t i = 0; instance->worker_running && i < count; i++) {
            const SubGhzRssiScanPoint* point = &points[i];
            if(!point->valid) continue;

            subghz_fa_worker_set_path_for_frequency(point->frequency);
            const int64_t tune_started_us = esp_timer_get_time();
            const bool tuned = subghz_rssi_scan_tune(point);
            stats_tune_us += (uint64_t)(esp_timer_get_time() - tune_started_us);
            stats_tunes++;
            if(!tuned) {
                stats_rx_fail++;
                continue;
            }

            const float rssi = subghz_rssi_scan_measure(settle_coarse_us, 1);
            if(rssi > rssi_coarse) {
                rssi_coarse = rssi;
                coarse_point = point;
            }
        }

        // Second stage: fine scan around the coarse hit, 58 kHz filter.
        // Steps reuse the coarse frequency's calibration (+/-300 kHz).
        float rssi_fine = SUBGHZ_RSSI_SCAN_RSSI_NONE;
        uint32_t frequency_fine = 0;
        if(instance->worker_running && coarse_point && rssi_coarse > instance->trigger_level) {
            subghz_frequency_analyzer_worker_load_registers(subghz_preset_ook_58khz);

            SubGhzRssiScanPoint point = *coarse_point;
            const int64_t fine_start =
                (int64_t)coarse_point->frequency - SUBGHZ_FA_FINE_HALF_SPAN;
            const int64_t fine_end = (int64_t)coarse_point->frequency + SUBGHZ_FA_FINE_HALF_SPAN;
            for(int64_t f = fine_start; instance->worker_running && f < fine_end;
                f += SUBGHZ_FA_FINE_STEP) {
                if(f <= 0 || !furi_hal_subghz_is_frequency_valid((uint32_t)f)) continue;

                subghz_rssi_scan_set_frequency(&point, (uint32_t)f);
                subghz_fa_worker_set_path_for_frequency(point.frequency);
                if(!subghz_rssi_scan_tune(&point)) {
                    stats_rx_fail++;
                    continue;
                }

                // Two readings so an OOK gap does not skew the peak position.
                const float rssi = subghz_rssi_scan_measure(settle_fine_us, 2);
                FURI_LOG_T(TAG, "#:%lu:%f", point.frequency, (double)rssi);
                if(rssi > rssi_fine) {
                    rssi_fine = rssi;
                    frequency_fine = point.frequency;
                }
            }

            subghz_frequency_analyzer_worker_load_registers(subghz_preset_ook_650khz);
            stats_fine++;
        }

        // Deliver: the fine result if it holds up, else the coarse one.
        // Every hit is published immediately, also while a previous signal is
        // still being held, so a new transmission replaces it right away.
        const uint32_t now = furi_get_tick();
        bool hit = true;
        if(rssi_fine > instance->trigger_level) {
            FURI_LOG_D(TAG, "=:%lu:%f", frequency_fine, (double)rssi_fine);
            hold_frequency = frequency_fine;
            hold_rssi = rssi_fine;
        } else if(coarse_point && rssi_coarse > instance->trigger_level) {
            FURI_LOG_D(TAG, "~:%lu:%f", coarse_point->frequency, (double)rssi_coarse);
            hold_frequency = coarse_point->frequency;
            hold_rssi = rssi_coarse;
        } else {
            hit = false;
        }

        if(hit) {
            holding = true;
            lost_sent = false;
            last_hit_at = now;
            const int64_t publish_started_us = esp_timer_get_time();
            subghz_frequency_analyzer_worker_publish(instance, hold_frequency, hold_rssi, true);
            stats_publish_us += (uint64_t)(esp_timer_get_time() - publish_started_us);
            stats_published++;
        } else if(holding) {
            const uint32_t elapsed = now - last_hit_at;
            if(elapsed >= SUBGHZ_FA_HOLD_MS) {
                holding = false;
                subghz_frequency_analyzer_worker_publish(instance, 0, 0, false);
            } else if(!lost_sent && elapsed >= SUBGHZ_FA_SIGNAL_LOST_MS) {
                lost_sent = true;
                subghz_frequency_analyzer_worker_publish(
                    instance, hold_frequency, hold_rssi, false);
            }
        }

        stats_cycles++;
        stats_cycle_us += (uint64_t)(esp_timer_get_time() - cycle_started_us);
        if(now - stats_at >= SUBGHZ_FA_STATS_MS) {
            /* Everything needed to tell scan cost from UI cost: scan wall time
             * per cycle, average tune time (SPI + bus waits), how often a fine
             * scan ran, how many view updates were pushed and what they cost the
             * scan thread, plus the CPU clock and insomnia level (expect 160 MHz
             * and >=1 while scanning; 80 MHz means DFS is throttling the scan). */
            FURI_LOG_I(
                TAG,
                "scan: %lu cycles/%lu ms, %lu us/cycle, tune avg %lu us, fine %lu, "
                "published %lu (avg %lu us), cpu %lu MHz, insomnia %u, rx_fail %lu",
                stats_cycles,
                now - stats_at,
                stats_cycles ? (uint32_t)(stats_cycle_us / stats_cycles) : 0,
                stats_tunes ? (uint32_t)(stats_tune_us / stats_tunes) : 0,
                stats_fine,
                stats_published,
                stats_published ? (uint32_t)(stats_publish_us / stats_published) : 0,
                (unsigned long)esp_rom_get_cpu_ticks_per_us(),
                (unsigned)furi_hal_power_insomnia_level(),
                (unsigned long)stats_rx_fail);
            if(stats_rx_fail) {
                FURI_LOG_W(
                    TAG,
                    "CC1101 did not enter RX %lu times in the last %lu ms",
                    (unsigned long)stats_rx_fail,
                    (unsigned long)(now - stats_at));
            }
            stats_rx_fail = 0;
            stats_at = now;
            stats_cycles = 0;
            stats_fine = 0;
            stats_published = 0;
            stats_tune_us = 0;
            stats_tunes = 0;
            stats_publish_us = 0;
            stats_cycle_us = 0;
        }
    }

    free(points);

    //Stop CC1101
    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();
    furi_hal_power_insomnia_exit();
    furi_hal_power_light_sleep_inhibit_exit();

    return 0;
}

SubGhzFrequencyAnalyzerWorker* subghz_frequency_analyzer_worker_alloc(void* context) {
    furi_assert(context);
    SubGhzFrequencyAnalyzerWorker* instance =
        calloc(1, sizeof(SubGhzFrequencyAnalyzerWorker));
    if(!instance) return NULL;

    instance->thread = furi_thread_alloc_ex(
        "SubGhzFAWorker", 4096, subghz_frequency_analyzer_worker_thread, instance);
    if(!instance->thread) {
        free(instance);
        return NULL;
    }
    // The scan busy-waits for RSSI settling; keep GUI, LEDs and input ahead of it.
    furi_thread_set_priority(instance->thread, FuriThreadPriorityLow);
    SubGhz* subghz = context;
    instance->setting = subghz_txrx_get_setting(subghz->txrx);
    instance->trigger_level = subghz->last_settings->frequency_analyzer_trigger;
    instance->worker_running = false;
    instance->pair_callback = NULL;
    instance->context = NULL;
    //instance->trigger_level = SUBGHZ_FREQUENCY_ANALYZER_THRESHOLD;
    return instance;
}

void subghz_frequency_analyzer_worker_free(SubGhzFrequencyAnalyzerWorker* instance) {
    if(!instance) return;

    if(instance->worker_running) {
        subghz_frequency_analyzer_worker_stop(instance);
    }
    furi_thread_free(instance->thread);
    free(instance);
}

void subghz_frequency_analyzer_worker_set_pair_callback(
    SubGhzFrequencyAnalyzerWorker* instance,
    SubGhzFrequencyAnalyzerWorkerPairCallback callback,
    void* context) {
    furi_assert(instance);
    furi_assert(context);
    instance->pair_callback = callback;
    instance->context = context;
}

void subghz_frequency_analyzer_worker_start(SubGhzFrequencyAnalyzerWorker* instance) {
    furi_assert(instance);
    furi_assert(!instance->worker_running);

    instance->worker_running = true;

    furi_thread_start(instance->thread);
}

void subghz_frequency_analyzer_worker_stop(SubGhzFrequencyAnalyzerWorker* instance) {
    furi_assert(instance);
    furi_assert(instance->worker_running);

    instance->worker_running = false;

    furi_thread_join(instance->thread);
}

bool subghz_frequency_analyzer_worker_is_running(SubGhzFrequencyAnalyzerWorker* instance) {
    furi_assert(instance);
    return instance->worker_running;
}

void subghz_frequency_analyzer_worker_set_trigger_level(
    SubGhzFrequencyAnalyzerWorker* instance,
    float value) {
    instance->trigger_level = value;
}

float subghz_frequency_analyzer_worker_get_trigger_level(SubGhzFrequencyAnalyzerWorker* instance) {
    return instance->trigger_level;
}

uint32_t subghz_frequency_analyzer_get_nearest_frequency(
    SubGhzFrequencyAnalyzerWorker* instance,
    uint32_t input) {
    uint32_t result = 0;
    uint64_t best_distance = UINT64_MAX;

    for(size_t i = 0; i < subghz_setting_get_frequency_count(instance->setting); i++) {
        uint32_t current = subghz_setting_get_frequency(instance->setting, i);
        if(current == 0) {
            continue;
        }
        uint64_t distance = current > input ? (uint64_t)current - input :
                                             (uint64_t)input - current;
        if(distance < best_distance) {
            best_distance = distance;
            result = current;
        }
    }

    return result;
}
