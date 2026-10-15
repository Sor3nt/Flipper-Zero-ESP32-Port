#pragma once

#include <furi_hal.h>
#include <stdint.h>
#include <stdbool.h>

/* Number of frequency bins per sweep. 128 bins give a detailed trace on the
 * 320 px color display (interpolated ~2.5 px/bin) while keeping one sweep fast
 * enough (~300 ms) for a live waterfall. */
#define SUBGHZ_SPECTRUM_BINS 128

typedef struct SubGhzSpectrumWorker SubGhzSpectrumWorker;

/** Delivers one completed sweep.
 *
 * @param context   user context
 * @param rssi      SUBGHZ_SPECTRUM_BINS RSSI values (dBm), bin 0 = lowest freq
 * @param center    center frequency (Hz) this sweep was taken at
 * @param span      span (Hz) this sweep covered
 */
typedef void (*SubGhzSpectrumWorkerSweepCallback)(
    void* context,
    const float* rssi,
    uint32_t center,
    uint32_t span);

SubGhzSpectrumWorker* subghz_spectrum_worker_alloc(void);

void subghz_spectrum_worker_free(SubGhzSpectrumWorker* instance);

void subghz_spectrum_worker_set_callback(
    SubGhzSpectrumWorker* instance,
    SubGhzSpectrumWorkerSweepCallback callback,
    void* context);

/** Set the frequency window to sweep. Thread-safe; effective on the next sweep. */
void subghz_spectrum_worker_set_window(
    SubGhzSpectrumWorker* instance,
    uint32_t center,
    uint32_t span);

void subghz_spectrum_worker_start(SubGhzSpectrumWorker* instance);

void subghz_spectrum_worker_stop(SubGhzSpectrumWorker* instance);

bool subghz_spectrum_worker_is_running(SubGhzSpectrumWorker* instance);
