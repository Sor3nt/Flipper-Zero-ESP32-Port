#pragma once

#include <gui/view.h>

typedef struct {
    char uuid_str[40];
    uint8_t properties;
    uint8_t value[128];
    uint16_t value_len;
    bool read_pending;
} BleWalkDetailModel;

View* ble_walk_detail_view_alloc(void);
void ble_walk_detail_view_free(View* view);
