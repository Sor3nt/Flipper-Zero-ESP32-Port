#pragma once

#include <gui/view.h>
#include <stdint.h>
#include <stdbool.h>

#define WLAN_PORTSCAN_MAX_OPEN 32
#define WLAN_PORTSCAN_ITEMS_ON_SCREEN 4

typedef struct {
    uint16_t port;
    char service[12];
    char banner[64]; // Service-Banner (nach Connect aus dem Socket gelesen)
} WlanPortscanEntry;

typedef struct {
    char target_ip[16];
    WlanPortscanEntry ports[WLAN_PORTSCAN_MAX_OPEN];
    uint8_t count;
    uint8_t selected;
    uint8_t window_offset;
    bool scanning;
    uint8_t progress;
    char status[32];
} WlanPortscanViewModel;

View* wlan_portscan_view_alloc(void);
void wlan_portscan_view_free(View* view);
uint8_t wlan_portscan_view_get_selected(View* view);
