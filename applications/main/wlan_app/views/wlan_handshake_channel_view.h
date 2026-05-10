#pragma once

#include <gui/view.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t count;             // Anzahl beobachteter BSSIDs (Beacons)
    uint8_t channel;
    uint8_t hs_complete_count; // Anzahl Targets mit M2+M3
    bool running;
    bool deauth_active;
    bool auto_mode;            // Soft-Button-Right Label: Auto/Stop
    bool hopping;              // wenn true: Deauth-Button wird ausgeblendet
    uint32_t deauth_frames;
    char latest_handshake_ssid[33];
} WlanHsChannelViewModel;

View* wlan_handshake_channel_view_alloc(void);
void wlan_handshake_channel_view_free(View* view);
