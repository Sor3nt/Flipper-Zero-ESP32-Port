#pragma once

#include <gui/view.h>
#include <stdint.h>

typedef struct {
    char status[24];
    char file[64];
    uint32_t speed_kbps;
    uint32_t done;
    uint32_t total;
    uint8_t percent;
} WlanSdUpdateViewModel;

View* wlan_sd_update_view_alloc(void);
void wlan_sd_update_view_free(View* view);

void wlan_sd_update_view_update(
    View* view,
    const char* status,
    uint8_t percent,
    uint32_t done,
    uint32_t total,
    const char* file,
    uint32_t speed_kbps);
