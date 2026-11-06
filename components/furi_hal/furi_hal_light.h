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

/** Reasons the WS2812 ring is kept dark. Independent: the ring stays dark
 *  while any of them is set. */
typedef enum {
    FuriHalLightSuspendUsbScreenOff = (1 << 0), /* Auto Off LED USB */
    FuriHalLightSuspendLightSleep = (1 << 1), /* Auto Light Sleep */
} FuriHalLightSuspend;

/** Called (outside any light lock) when the ring goes dark (first reason set)
 *  or may light again (last reason cleared), so the owner of the colour and
 *  effect can stop or re-apply it. Must not block. */
typedef void (*FuriHalLightSuspendCallback)(void* context, bool suspended);

/** Set or clear one suspend reason. The first reason sends one black frame;
 *  while any is set every other WS2812 call only updates the cached colour,
 *  so no writer can relight the LEDs. Both transitions call the suspend
 *  callback; without one, clearing the last reason pushes the cached colour.
 *  No-op on boards without an addressable LED ring. */
void furi_hal_light_set_ws2812_suspended(FuriHalLightSuspend reason, bool suspended);

/** True while any suspend reason (or the shutdown lock) keeps the ring dark. */
bool furi_hal_light_is_ws2812_suspended(void);

/** Register the suspend callback (one owner: the notification service). */
void furi_hal_light_set_ws2812_suspend_callback(FuriHalLightSuspendCallback callback, void* context);

/** Final black frame before power-off / deep sleep, then lock out every later
 *  WS2812 update until reboot. Returns true if the frame was sent (or there
 *  is no ring). */
bool furi_hal_light_shutdown_ws2812(void);

#ifdef __cplusplus
}
#endif
