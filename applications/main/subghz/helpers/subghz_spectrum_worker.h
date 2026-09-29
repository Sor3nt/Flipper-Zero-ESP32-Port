#pragma once

#include <furi_hal.h>
#include <stdint.h>
#include <stdbool.h>

/* Bounded measured bins, interpolated for display only. Sweep time includes
 * PLL calibration, dwell rounded to RTOS ticks, and shared-SPI contention. */
#define SUBGHZ_SPECTRUM_BINS 128
#define SUBGHZ_WATERFALL_BINS 64

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
    uint32_t span,
    uint16_t bins);

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
/** Configure while stopped: beta waterfall is 64 endpoint bins, 203 kHz RX filter. */
void subghz_spectrum_worker_set_spectrogram(SubGhzSpectrumWorker* instance, bool enabled);

void subghz_spectrum_worker_stop(SubGhzSpectrumWorker* instance);

bool subghz_spectrum_worker_is_running(SubGhzSpectrumWorker* instance);
void subghz_spectrum_worker_set_controls(SubGhzSpectrumWorker* instance, bool paused, uint16_t dwell_ms);
