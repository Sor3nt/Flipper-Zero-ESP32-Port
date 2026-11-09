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
    /* 1: before automatic light sleep, every connected pad gets a defined
     * sleep state and the board's buttons become GPIO wake sources.
     * 0 (default): light sleep exactly as before. */
    uint8_t auto_light_sleep;
    /* 1: on USB, the WS2812 ring turns off while the screen is off and comes
     * back with the screen. 0 (default): unchanged. */
    uint8_t led_usb_auto_off;
    /* 1: keep the board's VBUS 5 V output enabled until the user turns it off.
     * Default is enabled on ESP32-S3 and disabled on other targets. */
    uint8_t vbus_5v_enabled;
} PowerSettings;

#ifdef __cplusplus
extern "C" {
#endif

void power_settings_load(PowerSettings* settings);
void power_settings_save(const PowerSettings* settings);

#ifdef __cplusplus
}
#endif
