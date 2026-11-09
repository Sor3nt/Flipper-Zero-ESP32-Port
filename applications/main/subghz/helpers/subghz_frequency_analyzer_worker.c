#include "subghz_frequency_analyzer_worker.h"
#include "subghz_rssi_scan.h"
#include <lib/drivers/cc1101.h>
#include <furi_hal_subghz.h>
#include <furi_hal_power.h>

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

#define SUBGHZ_FA_FINE_HALF_SPAN 300000
#define SUBGHZ_FA_FINE_STEP 20000
#define SUBGHZ_FA_FINE_POINTS ((2 * SUBGHZ_FA_FINE_HALF_SPAN) / SUBGHZ_FA_FINE_STEP)
#define SUBGHZ_FA_SETTLE_SCALE 60000000UL
#define SUBGHZ_FA_BW_COARSE 650000UL
#define SUBGHZ_FA_BW_FINE 58000UL
#define SUBGHZ_FA_SIGNAL_LOST_MS 300
#define SUBGHZ_FA_HOLD_MS 1500
#define SUBGHZ_FA_RECAL_MS 30000

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
    FuriMutex* config_mutex;
    bool worker_running; // guarded by config_mutex
    SubGhzSetting* setting;
    size_t frequency_count;
    SubGhzRssiScanPoint* coarse_points;
    SubGhzRssiScanPoint fine_points[SUBGHZ_FA_FINE_POINTS];
    uint32_t fine_center;

    float trigger_level;

    SubGhzFrequencyAnalyzerWorkerPairCallback pair_callback;
    void* context;
};

static bool subghz_frequency_analyzer_worker_should_run(
    SubGhzFrequencyAnalyzerWorker* instance) {
    furi_mutex_acquire(instance->config_mutex, FuriWaitForever);
    const bool running = instance->worker_running;
    furi_mutex_release(instance->config_mutex);
    return running;
}

static void subghz_frequency_analyzer_worker_set_running(
    SubGhzFrequencyAnalyzerWorker* instance,
    bool running) {
    furi_mutex_acquire(instance->config_mutex, FuriWaitForever);
    instance->worker_running = running;
    furi_mutex_release(instance->config_mutex);
}

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

static void subghz_frequency_analyzer_worker_calibrate_all(
    SubGhzFrequencyAnalyzerWorker* instance) {
    size_t failed = 0;
    for(size_t i = 0; subghz_frequency_analyzer_worker_should_run(instance) &&
                      i < instance->frequency_count;
        i++) {
        const uint32_t frequency = subghz_setting_get_frequency(instance->setting, i);
        if(subghz_frequency_analyzer_worker_is_scannable(frequency) &&
           !subghz_rssi_scan_calibrate(frequency, &instance->coarse_points[i])) {
            failed++;
        }
    }
    if(failed) {
        FURI_LOG_W(TAG, "Initial CC1101 calibration failed for %u scan points", (unsigned)failed);
    }
}

/** Worker thread
 * 
 * @param context 
 * @return exit code 
 */


static void subghz_frequency_analyzer_worker_publish(
    SubGhzFrequencyAnalyzerWorker* instance,
    uint32_t frequency,
    float rssi,
    bool signal) {
    if(instance->pair_callback) {
        instance->pair_callback(instance->context, frequency, rssi, signal);
    }
}

static float subghz_frequency_analyzer_worker_get_trigger_snapshot(
    SubGhzFrequencyAnalyzerWorker* instance) {
    furi_mutex_acquire(instance->config_mutex, FuriWaitForever);
    const float trigger = instance->trigger_level;
    furi_mutex_release(instance->config_mutex);
    return trigger;
}

static int32_t subghz_frequency_analyzer_worker_thread(void* context) {
    SubGhzFrequencyAnalyzerWorker* instance = context;
    furi_hal_power_insomnia_enter();

    furi_hal_subghz_reset();
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    cc1101_flush_rx(&furi_hal_spi_bus_handle_subghz);
    cc1101_flush_tx(&furi_hal_spi_bus_handle_subghz);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_IOCFG0, CC1101IocfgHW);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_MDMCFG3, 0b01111111);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_AGCCTRL2, 0b00000111);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_AGCCTRL1, 0b00001000);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_AGCCTRL0, 0b00110000);
    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);
    furi_hal_subghz_set_path(FuriHalSubGhzPathIsolate);

    const uint32_t coarse_settle_us =
        subghz_rssi_scan_settle_us(SUBGHZ_FA_BW_COARSE, SUBGHZ_FA_SETTLE_SCALE);
    const uint32_t fine_settle_us =
        subghz_rssi_scan_settle_us(SUBGHZ_FA_BW_FINE, SUBGHZ_FA_SETTLE_SCALE);

    subghz_frequency_analyzer_worker_load_registers(subghz_preset_ook_650khz);
    subghz_frequency_analyzer_worker_calibrate_all(instance);

    size_t recal_index = 0;
    uint32_t last_recalibration = furi_get_tick();
    uint32_t fine_center = 0;
    memset(instance->fine_points, 0, sizeof(instance->fine_points));

    bool holding = false;
    bool lost_sent = false;
    uint32_t last_hit_tick = 0;
    uint32_t held_frequency = 0;
    float held_rssi = 0;

    while(subghz_frequency_analyzer_worker_should_run(instance)) {
        furi_delay_ms(5);
        if(!subghz_frequency_analyzer_worker_should_run(instance)) break;

        /* Refresh a few FSCAL entries per cycle. This avoids a long periodic
         * full-band pause while tracking temperature and supply drift. */
        if(furi_get_tick() - last_recalibration >= SUBGHZ_FA_RECAL_MS) {
            for(size_t n = 0; n < 4 &&
                               subghz_frequency_analyzer_worker_should_run(instance);
                n++) {
                if(recal_index >= instance->frequency_count) recal_index = 0;
                const uint32_t frequency =
                    subghz_setting_get_frequency(instance->setting, recal_index);
                if(subghz_frequency_analyzer_worker_is_scannable(frequency)) {
                    (void)subghz_rssi_scan_calibrate(
                        frequency, &instance->coarse_points[recal_index]);
                }
                if(++recal_index >= instance->frequency_count) {
                    recal_index = 0;
                    last_recalibration = furi_get_tick();
                    break;
                }
            }
        }

        float trigger = subghz_frequency_analyzer_worker_get_trigger_snapshot(instance);
        float coarse_rssi = SUBGHZ_RSSI_SCAN_RSSI_NONE;
        SubGhzRssiScanPoint* coarse_point = NULL;

        for(size_t i = 0; subghz_frequency_analyzer_worker_should_run(instance) &&
                          i < instance->frequency_count;
            i++) {
            const uint32_t requested = subghz_setting_get_frequency(instance->setting, i);
            if(!subghz_frequency_analyzer_worker_is_scannable(requested)) continue;

            SubGhzRssiScanPoint* point = &instance->coarse_points[i];
            if(!point->valid && !subghz_rssi_scan_calibrate(requested, point)) continue;
            subghz_fa_worker_set_path_for_frequency(point->frequency);
            if(!subghz_rssi_scan_tune(point)) {
                point->valid = false;
                continue;
            }
            const float rssi = subghz_rssi_scan_measure(coarse_settle_us, 1);
            if(rssi > coarse_rssi) {
                coarse_rssi = rssi;
                coarse_point = point;
            }
        }

        float fine_rssi = SUBGHZ_RSSI_SCAN_RSSI_NONE;
        uint32_t fine_frequency = 0;
        if(subghz_frequency_analyzer_worker_should_run(instance) && coarse_point &&
           coarse_rssi > trigger) {
            if(fine_center != coarse_point->frequency) {
                memset(instance->fine_points, 0, sizeof(instance->fine_points));
                fine_center = coarse_point->frequency;
            }

            subghz_frequency_analyzer_worker_load_registers(subghz_preset_ook_58khz);
            const int64_t start = (int64_t)fine_center - SUBGHZ_FA_FINE_HALF_SPAN;
            for(size_t i = 0; i < SUBGHZ_FA_FINE_POINTS &&
                               subghz_frequency_analyzer_worker_should_run(instance);
                i++) {
                const int64_t requested64 = start + (int64_t)i * SUBGHZ_FA_FINE_STEP;
                if(requested64 <= 0 || requested64 > UINT32_MAX) continue;
                const uint32_t requested = (uint32_t)requested64;
                if(!furi_hal_subghz_is_frequency_valid(requested)) continue;

                SubGhzRssiScanPoint* point = &instance->fine_points[i];
                if((!point->valid || point->requested_frequency != requested) &&
                   !subghz_rssi_scan_calibrate(requested, point)) {
                    continue;
                }
                subghz_fa_worker_set_path_for_frequency(point->frequency);
                if(!subghz_rssi_scan_tune(point)) {
                    point->valid = false;
                    continue;
                }
                const float rssi = subghz_rssi_scan_measure(fine_settle_us, 2);
                if(rssi > fine_rssi) {
                    fine_rssi = rssi;
                    fine_frequency = point->frequency;
                }
            }
            subghz_frequency_analyzer_worker_load_registers(subghz_preset_ook_650khz);
        }

        const uint32_t now = furi_get_tick();
        bool hit = false;
        float hit_rssi = coarse_rssi;
        uint32_t hit_frequency = coarse_point ? coarse_point->frequency : 0;
        if(fine_rssi > trigger) {
            hit = true;
            hit_rssi = fine_rssi;
            hit_frequency = fine_frequency;
        } else if(coarse_point && coarse_rssi > trigger) {
            hit = true;
        }

        if(hit) {
            held_frequency = hit_frequency;
            held_rssi = hit_rssi;
            holding = true;
            lost_sent = false;
            last_hit_tick = now;
            subghz_frequency_analyzer_worker_publish(instance, held_frequency, held_rssi, true);
        } else if(holding) {
            const uint32_t elapsed = now - last_hit_tick;
            if(elapsed >= SUBGHZ_FA_HOLD_MS) {
                holding = false;
                subghz_frequency_analyzer_worker_publish(instance, 0, 0, false);
            } else if(!lost_sent && elapsed >= SUBGHZ_FA_SIGNAL_LOST_MS) {
                lost_sent = true;
                subghz_frequency_analyzer_worker_publish(instance, held_frequency, held_rssi, false);
            }
        }
    }

    free(instance->coarse_points);
    instance->coarse_points = NULL;
    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();
    furi_hal_power_insomnia_exit();
    return 0;
}

SubGhzFrequencyAnalyzerWorker* subghz_frequency_analyzer_worker_alloc(void* context) {
    furi_assert(context);
    SubGhzFrequencyAnalyzerWorker* instance = calloc(1, sizeof(SubGhzFrequencyAnalyzerWorker));
    if(!instance) return NULL;

    SubGhz* subghz = context;
    instance->setting = subghz_txrx_get_setting(subghz->txrx);
    instance->frequency_count = subghz_setting_get_frequency_count(instance->setting);
    if(!instance->frequency_count) {
        free(instance);
        return NULL;
    }
    instance->coarse_points = calloc(instance->frequency_count, sizeof(SubGhzRssiScanPoint));
    instance->config_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!instance->coarse_points || !instance->config_mutex) {
        free(instance->coarse_points);
        if(instance->config_mutex) furi_mutex_free(instance->config_mutex);
        free(instance);
        return NULL;
    }

    instance->thread = furi_thread_alloc_ex(
        "SubGhzFAWorker", 4096, subghz_frequency_analyzer_worker_thread, instance);
    if(!instance->thread) {
        free(instance->coarse_points);
        furi_mutex_free(instance->config_mutex);
        free(instance);
        return NULL;
    }
    instance->trigger_level = subghz->last_settings->frequency_analyzer_trigger;
    return instance;
}

void subghz_frequency_analyzer_worker_free(SubGhzFrequencyAnalyzerWorker* instance) {
    furi_assert(instance);

    if(instance->thread) furi_thread_free(instance->thread);
    free(instance->coarse_points);
    furi_mutex_free(instance->config_mutex);
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
    furi_assert(!subghz_frequency_analyzer_worker_should_run(instance));
    subghz_frequency_analyzer_worker_set_running(instance, true);

    furi_thread_start(instance->thread);
}

void subghz_frequency_analyzer_worker_stop(SubGhzFrequencyAnalyzerWorker* instance) {
    furi_assert(instance);
    furi_assert(subghz_frequency_analyzer_worker_should_run(instance));
    subghz_frequency_analyzer_worker_set_running(instance, false);

    furi_thread_join(instance->thread);
}

bool subghz_frequency_analyzer_worker_is_running(SubGhzFrequencyAnalyzerWorker* instance) {
    furi_assert(instance);
    return subghz_frequency_analyzer_worker_should_run(instance);
}

void subghz_frequency_analyzer_worker_set_trigger_level(
    SubGhzFrequencyAnalyzerWorker* instance,
    float value) {
    furi_assert(instance);
    furi_mutex_acquire(instance->config_mutex, FuriWaitForever);
    instance->trigger_level = value;
    furi_mutex_release(instance->config_mutex);
}

float subghz_frequency_analyzer_worker_get_trigger_level(SubGhzFrequencyAnalyzerWorker* instance) {
    furi_assert(instance);
    furi_mutex_acquire(instance->config_mutex, FuriWaitForever);
    float trigger_level = instance->trigger_level;
    furi_mutex_release(instance->config_mutex);
    return trigger_level;
}

uint32_t subghz_frequency_analyzer_get_nearest_frequency(
    SubGhzFrequencyAnalyzerWorker* instance,
    uint32_t input) {
    uint32_t prev_freq = 0;
    uint32_t result = 0;
    uint32_t current;

    for(size_t i = 0; i < subghz_setting_get_frequency_count(instance->setting); i++) {
        current = subghz_setting_get_frequency(instance->setting, i);
        if(current == 0) {
            continue;
        }
        if(current == input) {
            result = current;
            break;
        }
        if(current > input && prev_freq < input) {
            if(current - input < input - prev_freq) {
                result = current;
            } else {
                result = prev_freq;
            }
            break;
        }
        prev_freq = current;
    }

    return result;
}
