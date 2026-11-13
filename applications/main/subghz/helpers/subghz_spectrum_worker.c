#include "subghz_spectrum_worker.h"
#include "subghz_rssi_scan.h"
#include <lib/drivers/cc1101.h>
#include <furi_hal_power.h>
#include <furi_hal_subghz.h>

#include <furi.h>

#define TAG "SubGhzSpectrumWorker"

/* Per-bin RSSI settle time after switching to RX. 1 ms keeps the sweep snappy
 * for the live waterfall; the CC1101 AGC settles fast enough for a power view. */
#define SUBGHZ_SPECTRUM_SETTLE_SCALE 60000000UL

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
    bool worker_running; // guarded by mutex
    FuriMutex* mutex;

    uint32_t center;
    uint32_t span;
    uint32_t window_generation;

    SubGhzSpectrumWorkerSweepCallback callback;
    void* context;

    float rssi[SUBGHZ_SPECTRUM_BINS];
    SubGhzRssiScanPoint points[SUBGHZ_SPECTRUM_BINS];
};

static bool subghz_spectrum_worker_should_run(SubGhzSpectrumWorker* instance) {
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    const bool running = instance->worker_running;
    furi_mutex_release(instance->mutex);
    return running;
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

/** Pick the smallest CC1101 RX filter whose bandwidth still covers `step`. */
static uint8_t subghz_spectrum_worker_bw_nibble(uint32_t step, uint32_t* bandwidth) {
    for(size_t i = 0; i < COUNT_OF(subghz_spectrum_bw_table); i++) {
        if(subghz_spectrum_bw_table[i].bw >= step) {
            if(bandwidth) *bandwidth = subghz_spectrum_bw_table[i].bw;
            return subghz_spectrum_bw_table[i].nibble;
        }
    }
    if(bandwidth) *bandwidth = subghz_spectrum_bw_table[COUNT_OF(subghz_spectrum_bw_table) - 1].bw;
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

static bool subghz_spectrum_worker_window_is_current(
    SubGhzSpectrumWorker* instance,
    uint32_t generation) {
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    const bool current = instance->window_generation == generation;
    furi_mutex_release(instance->mutex);
    return current;
}

static int32_t subghz_spectrum_worker_thread(void* context) {
    SubGhzSpectrumWorker* instance = context;

    furi_hal_subghz_reset();
    subghz_spectrum_worker_setup_registers();
    furi_hal_subghz_set_path(FuriHalSubGhzPathIsolate);
    furi_hal_power_insomnia_enter();

    uint32_t active_generation = UINT32_MAX;

    while(subghz_spectrum_worker_should_run(instance)) {
        // Snapshot the requested window (the view may change it any time).
        furi_mutex_acquire(instance->mutex, FuriWaitForever);
        uint32_t center = instance->center;
        uint32_t span = instance->span;
        uint32_t generation = instance->window_generation;
        furi_mutex_release(instance->mutex);

        if(generation != active_generation) {
            memset(instance->points, 0, sizeof(instance->points));
            active_generation = generation;
        }

        uint32_t step = span / SUBGHZ_SPECTRUM_BINS;
        if(step == 0) step = 1;
        uint32_t f_start = center - span / 2;

        // Match the channel filter to the bin spacing.
        uint32_t bw_hz = 0;
        const uint8_t bw_nibble = subghz_spectrum_worker_bw_nibble(step, &bw_hz);
        uint8_t mdmcfg4 = (bw_nibble << 4) | 0b0111;
        furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
        cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_MDMCFG4, mdmcfg4);
        furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);

        for(size_t i = 0;
            i < SUBGHZ_SPECTRUM_BINS && subghz_spectrum_worker_should_run(instance) &&
            subghz_spectrum_worker_window_is_current(instance, generation);
            i++) {
            uint32_t freq = f_start + (uint32_t)(step * i) + step / 2;

            if(!furi_hal_subghz_is_frequency_valid(freq)) {
                instance->rssi[i] = -127.0f;
                continue;
            }

            subghz_spectrum_worker_set_path(freq);
            SubGhzRssiScanPoint* point = &instance->points[i];
            if((!point->valid || point->requested_frequency != freq) &&
               !subghz_rssi_scan_calibrate(freq, point)) {
                instance->rssi[i] = SUBGHZ_RSSI_SCAN_RSSI_NONE;
                continue;
            }
            if(!subghz_rssi_scan_tune(point)) {
                point->valid = false;
                instance->rssi[i] = SUBGHZ_RSSI_SCAN_RSSI_NONE;
                continue;
            }
            const uint32_t settle_us =
                subghz_rssi_scan_settle_us(bw_hz, SUBGHZ_SPECTRUM_SETTLE_SCALE);
            instance->rssi[i] = subghz_rssi_scan_measure(settle_us, 1);
        }

        if(subghz_spectrum_worker_should_run(instance) && instance->callback &&
           subghz_spectrum_worker_window_is_current(instance, generation)) {
            instance->callback(instance->context, instance->rssi, center, span);
        }

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

    instance->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    instance->thread =
        furi_thread_alloc_ex("SubGhzSpectrumWorker", 4096, subghz_spectrum_worker_thread, instance);
    if(!instance->mutex || !instance->thread) {
        if(instance->thread) furi_thread_free(instance->thread);
        if(instance->mutex) furi_mutex_free(instance->mutex);
        free(instance);
        return NULL;
    }

    instance->center = SUBGHZ_SPECTRUM_DEFAULT_CENTER;
    instance->span = SUBGHZ_SPECTRUM_DEFAULT_SPAN;
    instance->worker_running = false;
    instance->callback = NULL;
    instance->context = NULL;
    return instance;
}

void subghz_spectrum_worker_free(SubGhzSpectrumWorker* instance) {
    furi_assert(instance);
    if(instance->thread) furi_thread_free(instance->thread);
    if(instance->mutex) furi_mutex_free(instance->mutex);
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
    furi_assert(!subghz_spectrum_worker_should_run(instance));
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->worker_running = true;
    furi_mutex_release(instance->mutex);
    furi_thread_start(instance->thread);
}

void subghz_spectrum_worker_stop(SubGhzSpectrumWorker* instance) {
    furi_assert(instance);
    furi_assert(subghz_spectrum_worker_should_run(instance));
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->worker_running = false;
    furi_mutex_release(instance->mutex);
    furi_thread_join(instance->thread);
}

bool subghz_spectrum_worker_is_running(SubGhzSpectrumWorker* instance) {
    furi_assert(instance);
    return subghz_spectrum_worker_should_run(instance);
}
