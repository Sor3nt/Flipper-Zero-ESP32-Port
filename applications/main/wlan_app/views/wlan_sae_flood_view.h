#pragma once

#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <stdbool.h>
#include <stdint.h>

#define SAE_STATUS_BUFF_SIZE 33

// WPA3 SAE-Commit-Flood (Dragonblood-DoS): flutet Authentication-Frames
// (Algo=SAE, Seq=Commit) an WPA3/SAE-APs, um deren teure EC-Rechnung zu
// triggern. Nur eine Start/Stop-Taste (OK); Back schließt die Scene.
typedef struct {
    bool running;
    uint32_t frames;
    uint8_t current_channel;
    uint32_t elapsed_ms;
    uint16_t targets; // Anzahl erkannter SAE/WPA3-APs
    char status[SAE_STATUS_BUFF_SIZE];
} WlanSaeFloodModel;

View* wlan_sae_flood_view_alloc(void);
void wlan_sae_flood_view_free(View* view);
