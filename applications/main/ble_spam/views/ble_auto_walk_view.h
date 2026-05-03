#pragma once

#include <gui/view.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AutoWalkStatusIdle,
    AutoWalkStatusScan,
    AutoWalkStatusConnect,
    AutoWalkStatusDiscover,
    AutoWalkStatusRead,
    AutoWalkStatusDone,
} AutoWalkStatus;

typedef struct {
    uint16_t seen_count;
    uint16_t last_services;
    uint16_t last_chars;
    char last_name[32];
    char last_addr[18];
    AutoWalkStatus status;
    bool seen_full;
} BleAutoWalkModel;

View* ble_auto_walk_view_alloc(void);
void ble_auto_walk_view_free(View* view);
