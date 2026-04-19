#pragma once

#include <gui/view.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    bool running;
    bool deauth_active;
    bool has_beacon;
    bool has_m1;
    bool has_m2;
    bool has_m3;
    bool has_m4;
    bool complete;
    uint32_t eapol_count;
    uint32_t deauth_frames;
    uint32_t elapsed_sec;
} HandshakeViewModel;

View* handshake_view_alloc(void);
void handshake_view_free(View* view);
