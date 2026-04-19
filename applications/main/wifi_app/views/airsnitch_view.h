#pragma once

#include <gui/view.h>
#include <stdint.h>
#include <stdbool.h>

#define AIRSNITCH_MAX_HOSTS 64
#define AIRSNITCH_ITEMS_ON_SCREEN 4

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
} AirSnitchHost;

typedef struct {
    AirSnitchHost hosts[AIRSNITCH_MAX_HOSTS];
    uint8_t count;
    uint8_t selected;
    uint8_t window_offset;
    bool scanning;
    uint8_t progress; // 0-255 scan progress
    char status[32];  // status text
} AirSnitchViewModel;

View* airsnitch_view_alloc(void);
void airsnitch_view_free(View* view);
