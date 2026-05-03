#pragma once

#include <gui/view.h>
#include "../ble_tracker_hal.h"

typedef struct {
    uint8_t target_addr[6];
    TrackerKind target_kind;
    char target_name[24];
    int8_t rssi;
    uint32_t last_seen_ms;
    bool stale;
} TrackerGeigerModel;

View* tracker_geiger_view_alloc(void);
void tracker_geiger_view_free(View* view);
