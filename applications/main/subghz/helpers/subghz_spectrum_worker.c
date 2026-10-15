#include "subghz_spectrum_worker.h"
#include "subghz_spectrum_math.h"
#include <lib/drivers/cc1101.h>
#include <furi_hal_subghz.h>

#include <furi.h>
#include <esp_heap_caps.h>
#include <stdatomic.h>

#define TAG "SubGhzSpectrumWorker"

/* Initial settle matches the Bruce-beta waterfall reference. Actual dwell is
 * user adjustable and rounded to the RTOS tick; RF accuracy needs bench tests. */
#define SUBGHZ_SPECTRUM_SETTLE_MS 3

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

struct SubGhzSpectrumWorker {
    FuriThread* thread;
    atomic_bool worker_running;
    FuriMutex* mutex;

    uint32_t center;
    uint32_t span;
    bool paused;
    uint16_t dwell_ms;
    bool spectrogram;

    SubGhzSpectrumWorkerSweepCallback callback;
    void* context;

    float rssi[SUBGHZ_SPECTRUM_BINS];
};

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

/** Pick the smallest CC1101 RX filter whose bandwidth still covers `step`. */
static uint8_t subghz_spectrum_worker_bw_nibble(uint32_t step) {
    for(size_t i = 0; i < COUNT_OF(subghz_spectrum_bw_table); i++) {
        if(subghz_spectrum_bw_table[i].bw >= step) {
            return subghz_spectrum_bw_table[i].nibble;
        }
    }
    return 0x0; // step wider than the widest filter -> use 812 kHz
}

static void subghz_spectrum_worker_setup_registers(void) {
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    cc1101_flush_rx(&furi_hal_spi_bus_handle_subghz);
    cc1101_flush_tx(&furi_hal_spi_bus_handle_subghz);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_IOCFG0, CC1101IocfgHW);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_MDMCFG3, 0b01111111); // symbol rate
    // AGC config identical to the frequency analyzer for comparable RSSI values.
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_AGCCTRL2, 0b00000111);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_AGCCTRL1, 0b00001000);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_AGCCTRL0, 0b00110000);
    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);
}

static int32_t subghz_spectrum_worker_thread(void* context) {
    SubGhzSpectrumWorker* instance = context;

    furi_hal_subghz_reset();
    subghz_spectrum_worker_setup_registers();
    furi_hal_subghz_set_path(FuriHalSubGhzPathIsolate);

    while(instance->worker_running) {
        // Snapshot the requested window (the view may change it any time).
        furi_mutex_acquire(instance->mutex, FuriWaitForever);
        uint32_t center = instance->center;
        uint32_t span = instance->span;
        bool paused = instance->paused;
        uint16_t dwell_ms = instance->dwell_ms;
        furi_mutex_release(instance->mutex);
        if(paused) {
            furi_hal_subghz_idle();
            furi_delay_ms(20);
            continue;
        }

        uint16_t bins = instance->spectrogram ? SUBGHZ_WATERFALL_BINS : SUBGHZ_SPECTRUM_BINS;
        uint32_t step = span / bins;
        if(step == 0) step = 1;

        // Match the channel filter to the bin spacing.
        uint8_t bandwidth = instance->spectrogram ? 0x8 : subghz_spectrum_worker_bw_nibble(step);
        uint8_t mdmcfg4 = (bandwidth << 4) | 0b0111;
        furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
        cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_MDMCFG4, mdmcfg4);
        furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);

        for(size_t i = 0; i < bins && instance->worker_running; i++) {
            uint32_t freq = subghz_spectrum_bin_frequency(center, span, i, bins, instance->spectrogram);

            if(!subghz_spectrum_frequency_supported(freq) || !furi_hal_subghz_is_frequency_valid(freq)) {
                instance->rssi[i] = -127.0f;
                continue;
            }

            subghz_spectrum_worker_set_path(freq);

            furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
            cc1101_switch_to_idle(&furi_hal_spi_bus_handle_subghz);
            cc1101_set_frequency(&furi_hal_spi_bus_handle_subghz, freq);
            cc1101_calibrate(&furi_hal_spi_bus_handle_subghz);
            bool calibrated = cc1101_wait_status_state(&furi_hal_spi_bus_handle_subghz, CC1101StateIDLE, 10000);
            if(!calibrated) {
                furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);
                instance->rssi[i] = -127.0f;
                continue;
            }
            cc1101_switch_to_rx(&furi_hal_spi_bus_handle_subghz);
            furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);

            furi_delay_ms(dwell_ms);

            instance->rssi[i] = furi_hal_subghz_get_rssi();
        }

        if(instance->worker_running && instance->callback) {
            instance->callback(instance->context, instance->rssi, center, span, bins);
        }

        furi_delay_ms(1); // let the GUI thread run
    }

    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();
    return 0;
}

SubGhzSpectrumWorker* subghz_spectrum_worker_alloc(void) {
    SubGhzSpectrumWorker* instance = heap_caps_calloc(1, sizeof(SubGhzSpectrumWorker), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!instance) return NULL;
    instance->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!instance->mutex) { free(instance); return NULL; }
    instance->thread =
        furi_thread_alloc_ex("SubGhzSpectrumWorker", 4096, subghz_spectrum_worker_thread, instance);
    instance->worker_running = false;
    instance->center = SUBGHZ_SPECTRUM_DEFAULT_CENTER;
    instance->span = SUBGHZ_SPECTRUM_DEFAULT_SPAN;
    instance->dwell_ms = SUBGHZ_SPECTRUM_SETTLE_MS;
    instance->callback = NULL;
    instance->context = NULL;
    return instance;
}

void subghz_spectrum_worker_free(SubGhzSpectrumWorker* instance) {
    furi_assert(instance);
    furi_thread_free(instance->thread);
    furi_mutex_free(instance->mutex);
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

void subghz_spectrum_worker_set_spectrogram(SubGhzSpectrumWorker* instance, bool enabled) {
    furi_assert(instance && !instance->worker_running);
    instance->spectrogram = enabled;
}

void subghz_spectrum_worker_set_controls(SubGhzSpectrumWorker* instance, bool paused, uint16_t dwell_ms) {
    furi_assert(instance);
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->paused = paused;
    instance->dwell_ms = dwell_ms < 1 ? 1 : dwell_ms > 20 ? 20 : dwell_ms;
    furi_mutex_release(instance->mutex);
}
