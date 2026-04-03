#pragma once

#include <gui/view.h>
#include <gui/view_dispatcher.h>

typedef struct {
    char attack_name[24];
    char device_name[48];
    uint32_t packet_count;
    uint32_t delay_ms;
    bool running;
} BleSpamRunningModel;

View* ble_spam_view_alloc(void);
void ble_spam_view_free(View* view);
