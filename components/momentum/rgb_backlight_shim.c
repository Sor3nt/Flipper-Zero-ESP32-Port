#include "rgb_backlight.h"

#define RGB_SHIM_LED_COUNT 3

static RgbColor rgb_shim_led[RGB_SHIM_LED_COUNT];
static RGBBacklightRainbowMode rgb_shim_rainbow_mode;
static uint8_t rgb_shim_rainbow_speed;
static uint32_t rgb_shim_rainbow_interval;
static uint8_t rgb_shim_rainbow_saturation;

void rgb_backlight_load_settings(bool enabled) {
    (void)enabled;
}

void rgb_backlight_save_settings(void) {
}

void rgb_backlight_set_color(uint8_t index, const RgbColor* color) {
    if(index < RGB_SHIM_LED_COUNT && color) {
        rgb_shim_led[index] = *color;
    }
}

void rgb_backlight_get_color(uint8_t index, RgbColor* color) {
    if(index < RGB_SHIM_LED_COUNT && color) {
        *color = rgb_shim_led[index];
    }
}

void rgb_backlight_set_rainbow_mode(RGBBacklightRainbowMode rainbow_mode) {
    if(rainbow_mode < RGBBacklightRainbowModeCount) {
        rgb_shim_rainbow_mode = rainbow_mode;
    }
}

RGBBacklightRainbowMode rgb_backlight_get_rainbow_mode(void) {
    return rgb_shim_rainbow_mode;
}

void rgb_backlight_set_rainbow_speed(uint8_t rainbow_speed) {
    rgb_shim_rainbow_speed = rainbow_speed;
}

uint8_t rgb_backlight_get_rainbow_speed(void) {
    return rgb_shim_rainbow_speed;
}

void rgb_backlight_set_rainbow_interval(uint32_t rainbow_interval) {
    rgb_shim_rainbow_interval = rainbow_interval;
}

uint32_t rgb_backlight_get_rainbow_interval(void) {
    return rgb_shim_rainbow_interval;
}

void rgb_backlight_set_rainbow_saturation(uint8_t rainbow_saturation) {
    rgb_shim_rainbow_saturation = rainbow_saturation;
}

uint8_t rgb_backlight_get_rainbow_saturation(void) {
    return rgb_shim_rainbow_saturation;
}

void rgb_backlight_reconfigure(bool enabled) {
    (void)enabled;
}

void rgb_backlight_update(uint8_t brightness, bool forced) {
    (void)brightness;
    (void)forced;
}
