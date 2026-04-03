#pragma once

#include <gui/view.h>
#include <gui/view_dispatcher.h>

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    uint32_t frames_sent;
    bool running;
    bool channel_mode;  // true = channel attack, false = SSID attack
    uint8_t ap_count;   // number of APs found on channel (channel mode)
    bool scanned;       // true after scan completed (channel mode)
} DeautherModel;

View* deauther_view_alloc(void);
void deauther_view_free(View* view);
