#pragma once

#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <stdint.h>

#define AP_LIST_ITEMS_ON_SCREEN 4

/** Generic AP record for display. Must match WifiApRecord layout. */
typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t authmode;
    bool is_open;
    bool has_password;
} ApListRecord;

typedef struct {
    ApListRecord* records;
    uint16_t count;
    uint16_t selected;
    uint16_t window_offset;
} ApListModel;

View* ap_list_alloc(void);
void ap_list_free(View* view);

/** Set the view dispatcher for sending custom events. */
void ap_list_set_view_dispatcher(ViewDispatcher* vd);
