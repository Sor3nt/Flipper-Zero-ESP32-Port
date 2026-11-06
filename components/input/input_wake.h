/**
 * @file input_wake.h
 * Light-sleep GPIO wake for the board input drivers (Auto Light Sleep).
 *
 * While light sleep is allowed the input service calls the board's
 * target_input_idle_wait() instead of polling every 4 ms. The board arms its
 * button / encoder pins as level-triggered GPIO wake sources and interrupts
 * with input_wake_wait(); the first edge wakes the SoC, the ISR masks the
 * pins and releases the input thread, and normal debounced polling resumes.
 *
 * ESP32-S3: any GPIO can wake light sleep, level triggers only
 * (sleep_modes.rst). gpio_wakeup_enable() also keeps the pad's run-mode pull
 * active during sleep, so an armed pin never floats.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <driver/gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INPUT_WAKE_MAX_PINS 8

typedef struct {
    gpio_num_t pin;
    gpio_int_type_t level; /* GPIO_INTR_LOW_LEVEL or GPIO_INTR_HIGH_LEVEL */
} InputWakePin;

/** Register the wake ISR on the pins and enable GPIO light-sleep wake.
 *  Call once from target_input_init(). Returns false (and the input service
 *  keeps polling) if anything is unavailable. */
bool input_wake_init(const gpio_num_t* pins, size_t count);

/** Arm the pins at the given levels and block until one fires or the timeout
 *  elapses, then disarm them. A level already present when armed fires at
 *  once, so nothing is lost between the idle check and arming.
 *  @return false if input_wake_init() did not succeed (nothing waited) */
bool input_wake_wait(const InputWakePin* pins, size_t count, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
