#pragma once

#include <gui/view.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t progress; /* 0..100 */
    uint32_t sweeps;
    bool hardware_ok;
} Nrf24ScanModel;

View* nrf24_scan_view_alloc(void);
void nrf24_scan_view_free(View* view);
