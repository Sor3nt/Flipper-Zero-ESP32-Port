#pragma once

// Scanner BLE wardriving tetap dikompilasi dari modul wlan_app (API C murni,
// di-link ke dalam firmware yang sama). View & scene-nya kini berada di app
// Bluetooth (ble_spam), bukan lagi di menu WiFi.
#include "../../wlan_app/wlan_ble_scanner.h"
#include <gui/view.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool           running;
    uint16_t       count;
    uint16_t       total;
    BleDeviceView  devices[BLE_SCAN_VIEW_ENTRIES];
    uint16_t       cursor;
    bool           detail_mode;
    bool           exit_pending;
    bool           error; // true kalau bring-up scanner gagal → tampilkan pesan
    BleDeviceEntry detail;
    bool           detail_valid;
} BleWardrivingModel;

View* ble_wardriving_view_alloc(void);
void  ble_wardriving_view_free(View* view);
