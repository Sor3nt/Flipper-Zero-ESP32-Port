#pragma once

#include <gui/view.h>
#include <stdint.h>
#include <stdbool.h>

#define HS_CHANNEL_VIEW_MAX 16
#define HS_CHANNEL_ITEMS_ON_SCREEN 4

typedef struct {
    char ssid[11]; // max 10 chars + null
    bool has_m1;
    bool has_m2;
    bool has_m3;
    bool has_m4;
    bool has_beacon;
    bool complete;
} HsChannelEntry;

typedef struct {
    HsChannelEntry entries[HS_CHANNEL_VIEW_MAX];
    uint8_t count;
    uint8_t selected;
    uint8_t window_offset;
    uint8_t channel;
    uint8_t hs_complete_count; // number of completed handshakes
    bool running;
} HsChannelViewModel;

View* handshake_channel_view_alloc(void);
void handshake_channel_view_free(View* view);
