#pragma once

#include <stdbool.h>

typedef struct PowerOff PowerOff;

typedef enum {
    PowerOffResponseDefault,
    PowerOffResponseOk,
    PowerOffResponseCancel,
    PowerOffResponseHide,
} PowerOffResponse;

#include <gui/view.h>

PowerOff* power_off_alloc(void);

void power_off_free(PowerOff* power_off);

View* power_off_get_view(PowerOff* power_off);

void power_off_set_time_left(PowerOff* power_off, uint8_t time_left);

void power_off_set_manual(PowerOff* power_off, bool manual);

void power_off_reset_response(PowerOff* power_off);

PowerOffResponse power_off_get_response(PowerOff* power_off);
