#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <toolbox/colors.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RGBBacklightRainbowModeOff,
    RGBBacklightRainbowModeWave,
    RGBBacklightRainbowModeSolid,
    RGBBacklightRainbowModeCount,
} RGBBacklightRainbowMode;

void rgb_backlight_load_settings(bool enabled);
void rgb_backlight_save_settings(void);

void rgb_backlight_set_color(uint8_t index, const RgbColor* color);
void rgb_backlight_get_color(uint8_t index, RgbColor* color);

void rgb_backlight_set_rainbow_mode(RGBBacklightRainbowMode rainbow_mode);
RGBBacklightRainbowMode rgb_backlight_get_rainbow_mode(void);

void rgb_backlight_set_rainbow_speed(uint8_t rainbow_speed);
uint8_t rgb_backlight_get_rainbow_speed(void);

void rgb_backlight_set_rainbow_interval(uint32_t rainbow_interval);
uint32_t rgb_backlight_get_rainbow_interval(void);

void rgb_backlight_set_rainbow_saturation(uint8_t rainbow_saturation);
uint8_t rgb_backlight_get_rainbow_saturation(void);

void rgb_backlight_reconfigure(bool enabled);
void rgb_backlight_update(uint8_t brightness, bool forced);

#ifdef __cplusplus
}
#endif
