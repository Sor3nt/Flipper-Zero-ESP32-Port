#pragma once

#include <gui/view.h>
#include "../ble_walk_hal.h"

#define WALK_SCAN_ITEMS_ON_SCREEN 4

typedef enum {
    WalkScanStatusNone,
    WalkScanStatusConnecting,
    WalkScanStatusConnected,
    WalkScanStatusFailed,
} WalkScanStatus;

typedef struct {
    BleWalkDevice devices[BLE_WALK_MAX_DEVICES];
    uint16_t count;
    uint16_t selected;
    uint16_t window_offset;
    bool scanning;
    WalkScanStatus connect_status;
} BleWalkScanModel;

View* ble_walk_scan_view_alloc(void);
void ble_walk_scan_view_free(View* view);
