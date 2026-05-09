/**
 * @file furi_hal_light.h
 * Light control HAL API (ESP32 stub)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <furi_hal_resources.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Init light driver
 */
void furi_hal_light_init(void);

/** Set light value
 *
 * @param      light  Light
 * @param      value  light brightness [0-255]
 */
void furi_hal_light_set(Light light, uint8_t value);

/** Start hardware LED blinking mode
 *
 * @param      light  Light
 * @param      brightness  light brightness [0-255]
 * @param      on_time  LED on time in ms
 * @param      period  LED blink period in ms
 */
void furi_hal_light_blink_start(Light light, uint8_t brightness, uint16_t on_time, uint16_t period);

/** Stop hardware LED blinking mode
 */
void furi_hal_light_blink_stop(void);

/** Set color in hardware LED blinking mode
 *
 * @param      light  Light
 */
void furi_hal_light_blink_set_color(Light light);

/** Execute sequence
 *
 * @param      sequence  Sequence to execute
 */
void furi_hal_light_sequence(const char* sequence);

/** Set RGB color on ALL WS2812 pixels and push immediately.
 *  No-op on boards without an addressable LED ring. */
void furi_hal_light_set_rgb_all(uint8_t r, uint8_t g, uint8_t b);

/** Set RGB color on a specific WS2812 pixel WITHOUT pushing.
 *  Call furi_hal_light_refresh() after staging all pixels you want.
 *  No-op on boards without an addressable LED ring. */
void furi_hal_light_set_pixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b);

/** Push staged pixel data to the strip.
 *  No-op on boards without an addressable LED ring. */
void furi_hal_light_refresh(void);

/** Number of WS2812 pixels on this board (0 if no addressable ring). */
uint16_t furi_hal_light_pixel_count(void);

#ifdef __cplusplus
}
#endif
