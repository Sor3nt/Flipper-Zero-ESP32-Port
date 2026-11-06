#pragma once

#include "../wlan_live_scanner.h"
#include <gui/view.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool           running;
    uint16_t       count;
    LiveApEntry    aps[LIVE_SCAN_MAX_APS];
    uint16_t       cursor;
    bool           hopping;
    uint8_t        current_channel;
    LiveSortMode   sort;
    LiveFilterMode filter;
    bool           detail_mode;
} WlanLiveScanModel;

View* wlan_live_scan_view_alloc(void);
void  wlan_live_scan_view_free(View* view);
