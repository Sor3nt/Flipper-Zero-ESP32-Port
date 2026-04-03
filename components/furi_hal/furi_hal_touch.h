/**
 * @file furi_hal_touch.h
 * Touch HAL API for CST816S capacitive touch controller
 * (Waveshare ESP32-C6-LCD-1.9)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize CST816S touch controller (I2C bus, reset, interrupt pin).
 *  Safe to call even if touch hardware is absent — will log warning and return. */
void furi_hal_touch_init(void);

/** Check if a finger is currently touching the screen.
 *  Reads CST816S registers via I2C.
 *  @return true if finger detected */
bool furi_hal_touch_is_pressed(void);

/** Read touch coordinates.
 *  @param[out] x  X coordinate (0-319 in landscape)
 *  @param[out] y  Y coordinate (0-169 in landscape) */
void furi_hal_touch_get_xy(uint16_t* x, uint16_t* y);

/** CST816S gesture IDs */
typedef enum {
    TouchGestureNone = 0x00,
    TouchGestureSlideUp = 0x01,
    TouchGestureSlideDown = 0x02,
    TouchGestureSlideLeft = 0x03,
    TouchGestureSlideRight = 0x04,
    TouchGestureSingleClick = 0x05,
    TouchGestureDoubleClick = 0x0B,
    TouchGestureLongPress = 0x0C,
} TouchGesture;

/** Combined touch data from a single I2C read */
typedef struct {
    TouchGesture gesture;
    uint8_t finger_count;
    uint16_t x;
    uint16_t y;
} TouchData;

/** Read all touch data in a single I2C transaction.
 *  @param[out] data  Touch data struct to fill
 *  @return true if I2C read succeeded */
bool furi_hal_touch_read(TouchData* data);

/** Read gesture ID from CST816S.
 *  @return gesture id (0=None, 1=SlideUp, 2=SlideDown, 3=SlideLeft, 4=SlideRight, 5=SingleClick) */
uint8_t furi_hal_touch_get_gesture(void);

/** Set the thread ID that will receive interrupt flags on touch events.
 *  @param thread_id  FuriThreadId to notify (flag bit 0 will be set) */
void furi_hal_touch_set_notify_thread(void* thread_id);

/** Check if the touch INT pin is active (low = touch event pending).
 *  Does NOT use I2C — safe to call when CST816S is sleeping.
 *  @return true if INT pin is low (touch active) */
bool furi_hal_touch_int_active(void);

#ifdef __cplusplus
}
#endif
