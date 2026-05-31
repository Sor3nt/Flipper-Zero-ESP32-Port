/**
 * @file rgb_backlight.h
 * @brief Stub RGB backlight driver for ESP32 port
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <momentum/settings.h>
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

static inline void rgb_backlight_load_settings(bool enabled) { (void)enabled; }
static inline void rgb_backlight_save_settings(void) {}
static inline void rgb_backlight_set_color(uint8_t index, const RgbColor* color) { (void)index; (void)color; }
static inline void rgb_backlight_get_color(uint8_t index, RgbColor* color) { (void)index; (void)color; }
static inline void rgb_backlight_set_rainbow_mode(RGBBacklightRainbowMode mode) { (void)mode; }
static inline RGBBacklightRainbowMode rgb_backlight_get_rainbow_mode(void) { return RGBBacklightRainbowModeOff; }
static inline void rgb_backlight_set_rainbow_speed(uint8_t speed) { (void)speed; }
static inline uint8_t rgb_backlight_get_rainbow_speed(void) { return 0; }
static inline void rgb_backlight_set_rainbow_interval(uint32_t interval) { (void)interval; }
static inline uint32_t rgb_backlight_get_rainbow_interval(void) { return 0; }
static inline void rgb_backlight_set_rainbow_saturation(uint8_t sat) { (void)sat; }
static inline uint8_t rgb_backlight_get_rainbow_saturation(void) { return 0; }
static inline void rgb_backlight_reconfigure(bool enabled) { (void)enabled; }
static inline void rgb_backlight_update(uint8_t brightness, bool forced) { (void)brightness; (void)forced; }

#ifdef __cplusplus
}
#endif
