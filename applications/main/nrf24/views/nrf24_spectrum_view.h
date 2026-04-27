#pragma once

#include <gui/view.h>

#define NRF24_SPECTRUM_CHANNELS 80

typedef struct {
    uint8_t levels[NRF24_SPECTRUM_CHANNELS];
    uint16_t sweep_count;
    bool running;
    bool hardware_ok;
} Nrf24SpectrumModel;

View* nrf24_spectrum_view_alloc(void);
void nrf24_spectrum_view_free(View* view);
