#pragma once

#include <gui/view.h>

#define NRF24_SPECTRUM_CHANNELS 80

typedef struct Nrf24Spectrum Nrf24Spectrum;

Nrf24Spectrum* nrf24_spectrum_alloc(void);
void nrf24_spectrum_free(Nrf24Spectrum* instance);
View* nrf24_spectrum_get_view(Nrf24Spectrum* instance);

/** Take over the color display and start the channel-sweep + render threads.
 * Called from the scene's on_enter. */
void nrf24_spectrum_start(Nrf24Spectrum* instance);

/** Stop the sweep + render threads and release the display. Called from the
 * scene's on_exit. Safe to call even if start() was never called. */
void nrf24_spectrum_stop(Nrf24Spectrum* instance);
