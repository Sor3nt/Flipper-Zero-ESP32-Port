#pragma once

#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <stdbool.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define DEAUTH_SSID_BUFF_SIZE 33

typedef struct {
    bool running;
    uint32_t frames;
    uint8_t current_channel;
    uint32_t elapsed_ms;
    char target_ssid[DEAUTH_SSID_BUFF_SIZE];
} WlanDeauthAllModel;

View* wlan_deauth_all_view_alloc(void);
void wlan_deauth_all_view_free(View* view);
