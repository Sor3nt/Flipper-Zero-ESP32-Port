#pragma once

#include <gui/view.h>
#include <stdint.h>
#include <stdbool.h>

#define NETSCAN_MAX_HOSTS 64
#define NETSCAN_ITEMS_ON_SCREEN 4

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
} NetscanHost;

typedef struct {
    char own_ip[16];
    NetscanHost hosts[NETSCAN_MAX_HOSTS];
    uint8_t count;
    uint8_t selected;
    uint8_t window_offset;
    bool scanning;
    uint8_t progress;
    char status[32];
} NetscanViewModel;

View* netscan_view_alloc(void);
void netscan_view_free(View* view);
