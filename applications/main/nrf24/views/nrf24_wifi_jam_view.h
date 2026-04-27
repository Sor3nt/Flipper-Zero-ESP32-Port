#pragma once

#include <gui/view.h>

typedef struct {
    char ssid[33];
    uint8_t wifi_channel;
    uint8_t current_nrf_ch;
    uint8_t nrf_ch_min;
    uint8_t nrf_ch_max;
    uint32_t hop_count;
    bool running;
    bool hardware_ok;
} Nrf24WifiJamModel;

typedef enum {
    Nrf24WifiJamEventToggle = 1,
} Nrf24WifiJamEvent;

View* nrf24_wifi_jam_view_alloc(void);
void nrf24_wifi_jam_view_free(View* view);
