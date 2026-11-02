#pragma once

#include <stdint.h>
#include <stdbool.h>

#define POWER_SETTINGS_INTERVAL_MIN_MINUTES (1U)
#define POWER_SETTINGS_INTERVAL_MAX_MINUTES (20U)
#define POWER_SETTINGS_DEFAULT_DEEP_SLEEP_USB_INTERVAL_MINUTES (10U)
#define POWER_SETTINGS_DEFAULT_LIGHT_SLEEP_SHIP_INTERVAL_MINUTES (10U)

typedef enum {
    PowerOffModeDeepSleep = 0, /* default: ESP32 deep sleep, ~µA draw, wakes on button */
    PowerOffModePowerOff = 1, /* real power-off via charger ship mode (battery only) */
} PowerOffMode;

typedef struct {
    uint32_t auto_poweroff_delay_ms;
    uint8_t charge_supress_percent;
    uint8_t off_mode; /* PowerOffMode */
    uint32_t light_sleep_delay_ms;
    bool light_sleep_enabled;
    bool deep_sleep_usb_check_enabled;
    uint8_t deep_sleep_usb_check_interval_minutes;
    bool light_sleep_ship_mode_enabled;
    uint8_t light_sleep_ship_mode_interval_minutes;
} PowerSettings;

#ifdef __cplusplus
extern "C" {
#endif

void power_settings_load(PowerSettings* settings);
void power_settings_save(const PowerSettings* settings);

#ifdef __cplusplus
}
#endif
