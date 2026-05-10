#pragma once

#include <gui/view.h>
#include <gui/view_dispatcher.h>

typedef struct {
    char title[24];   // Header-Titel; Default "Deauth" wenn leer
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    uint8_t target_count; // Aktuell selektierte Targets aus dem Picker (0 = keine)
    uint32_t frames_sent;
    bool running;       // OK gehalten ODER Auto aktiv
    bool auto_mode;     // Soft-Button rechts → "Auto" / "Stop"
    bool channel_mode;  // true → Up/Down ändern Channel, kein Targets-Soft-Button
    bool scanning;      // true → Worker scannt gerade Channel-APs
    char status_msg[40]; // Temporäre Statuszeile (z.B. "No APs on channel"), "" = leer
} WlanDeautherModel;

View* wlan_deauther_view_alloc(void);
void wlan_deauther_view_free(View* view);
