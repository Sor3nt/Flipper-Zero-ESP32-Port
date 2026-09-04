#pragma once

#include <gui/view.h>
#include "../helpers/subghz_custom_event.h"

typedef struct SubGhzSpectrum SubGhzSpectrum;

typedef void (*SubGhzSpectrumCallback)(SubGhzCustomEvent event, void* context);

void subghz_spectrum_set_callback(
    SubGhzSpectrum* instance,
    SubGhzSpectrumCallback callback,
    void* context);

SubGhzSpectrum* subghz_spectrum_alloc(void);

void subghz_spectrum_free(SubGhzSpectrum* instance);

View* subghz_spectrum_get_view(SubGhzSpectrum* instance);

/** Take over the color display and start the sweep + render threads. Called
 * from the scene's on_enter. */
void subghz_spectrum_start(SubGhzSpectrum* instance);

/** Stop the render + sweep threads and release the display. Called from the
 * scene's on_exit. Safe to call even if start() was never called. */
void subghz_spectrum_stop(SubGhzSpectrum* instance);
