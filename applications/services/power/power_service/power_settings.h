#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PowerOffModeDeepSleep = 0, /* default: ESP32 deep sleep, ~µA draw, wakes on button */
    PowerOffModePowerOff = 1, /* real power-off via charger ship mode (battery only) */
} PowerOffMode;

typedef struct {
    uint32_t auto_poweroff_delay_ms;
    uint8_t charge_supress_percent;
    uint8_t off_mode; /* PowerOffMode */
} PowerSettings;

#ifdef __cplusplus
extern "C" {
#endif

void power_settings_load(PowerSettings* settings);
void power_settings_save(const PowerSettings* settings);

#ifdef __cplusplus
}
#endif
