#pragma once

#include <gui/view.h>
#include "../ble_tracker_hal.h"

#define TRACKER_LIST_ITEMS_ON_SCREEN 4

typedef struct {
    TrackerDevice devices[TRACKER_MAX_DEVICES];
    uint16_t count;
    uint16_t selected;
    uint16_t window_offset;
} TrackerListModel;

View* tracker_list_view_alloc(void);
void tracker_list_view_free(View* view);
