#pragma once

#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool running;
    uint8_t channel;      // manuell gewählter Channel (1..13)
    uint32_t frames;      // gesendete Deauth/Disassoc-Frames
    uint8_t target_count; // gescannte APs auf dem aktuellen Channel
} WlanSmartDeauthModel;

View* wlan_smart_deauth_view_alloc(void);
void wlan_smart_deauth_view_free(View* view);
