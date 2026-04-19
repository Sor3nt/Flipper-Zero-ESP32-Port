#pragma once

#include <gui/view.h>

typedef struct {
    uint32_t packets;
    uint32_t bytes;
    uint8_t channel;
    uint32_t elapsed_sec;
    bool running;
} SnifferViewModel;

View* sniffer_view_alloc(void);
void sniffer_view_free(View* view);
