#pragma once

#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <stdbool.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define DEAUTH_SSID_BUFF_SIZE 33

// Urutan fokus kontrol di layar (dipindah pakai Up/Down pada T-Embed atau
// Left/Right pada board lain). Durasi hanya relevan saat Auto Scan aktif, tapi
// tetap bisa diatur kapan saja.
typedef enum {
    DeauthAllFocusStart = 0, // tombol Start/Stop
    DeauthAllFocusAuto,      // tombol Auto Scan (on/off)
    DeauthAllFocusDuration,  // label durasi (OK → masuk mode edit)
    DeauthAllFocusCount,
} DeauthAllFocus;

typedef struct {
    bool running;
    uint32_t frames;
    uint8_t current_channel;
    uint32_t elapsed_ms;
    char target_ssid[DEAUTH_SSID_BUFF_SIZE];

    // --- kontrol UI ---
    uint8_t focus;             // DeauthAllFocus
    bool editing_duration;     // true → sedang memilih durasi (nav = ubah nilai)
    bool auto_scan_enabled;    // status toggle Auto Scan (cermin dari settings)
    uint8_t interval_s;        // 10..60 (kelipatan 10)
    uint8_t interval_s_backup; // nilai sebelum edit, untuk dibatalkan via Back
} WlanDeauthAllModel;

View* wlan_deauth_all_view_alloc(void);
void wlan_deauth_all_view_free(View* view);
