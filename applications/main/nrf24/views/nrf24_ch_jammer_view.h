#pragma once

#include <gui/view.h>

typedef struct {
    uint8_t channel;
    uint8_t min_channel;
    uint8_t max_channel;
    bool running;
    bool hardware_ok;
} Nrf24ChJammerModel;

typedef enum {
    Nrf24ChJammerEventToggle = 1,
    Nrf24ChJammerEventChannelUp,
    Nrf24ChJammerEventChannelDown,
} Nrf24ChJammerEvent;

View* nrf24_ch_jammer_view_alloc(void);
void nrf24_ch_jammer_view_free(View* view);
