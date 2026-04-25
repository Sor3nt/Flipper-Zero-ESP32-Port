#pragma once

#include <gui/view.h>
#include <stdint.h>
#include <stdbool.h>

#define PORTSCAN_MAX_OPEN 32
#define PORTSCAN_ITEMS_ON_SCREEN 4

typedef struct {
    uint16_t port;
    char service[12];
} PortscanEntry;

typedef struct {
    char target_ip[16];
    PortscanEntry ports[PORTSCAN_MAX_OPEN];
    uint8_t count;
    uint8_t selected;
    uint8_t window_offset;
    bool scanning;
    uint8_t progress;
    char status[32];
} PortscanViewModel;

View* portscan_view_alloc(void);
void portscan_view_free(View* view);
