/**
 * @file furi_hal_power_sleep.h
 * Defined pad states for light sleep and deep sleep (internal to furi_hal).
 *
 * Every board gets a pin table derived from its board header (LCD, SD, radio
 * chip selects, I2C, IR, buttons, ...). A board may append its own entries
 * with FURI_HAL_POWER_DEEP_SLEEP_EXTRA_PINS / FURI_HAL_POWER_LIGHT_SLEEP_EXTRA_PINS;
 * a later entry for the same pin overrides an earlier one.
 */

#pragma once

#include <stdbool.h>
#include <sdkconfig.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Deep-sleep pad state. */
typedef enum {
    FuriHalPowerPinLow, /* output LOW, latched */
    FuriHalPowerPinHigh, /* output HIGH, latched */
    FuriHalPowerPinPullUp, /* input + pull-up, latched */
    FuriHalPowerPinPullDown, /* input + pull-down, latched */
    FuriHalPowerPinExternal, /* driven / biased externally (or wake pin): left untouched */
} FuriHalPowerPinMode;

/** Light-sleep pad state (sleep-mode pad registers only; awake config untouched). */
typedef enum {
    FuriHalPowerLsPinPullUp, /* input/output off, pull-up */
    FuriHalPowerLsPinPullDown, /* input/output off, pull-down */
    FuriHalPowerLsPinNoPull, /* input/output off, driven by the other chip */
    FuriHalPowerLsPinKeepOutput, /* keep driving the current level (power enables) */
} FuriHalPowerLsPinMode;

/** Release pad holds left over from deep sleep (or a reset after isolation).
 *  Runs from an early constructor and again from furi_hal_power_init(). */
void furi_hal_power_sleep_release_holds(void);

/** Log every usable GPIO that has no deep-sleep state. */
void furi_hal_power_sleep_audit_pins(void);

/** Drive and latch every deep-sleep table pin.
 *  @return true if a digital (non-RTC) pad needs the global deep-sleep hold */
bool furi_hal_power_sleep_isolate_pins(void);

/** Enter deep sleep from a task with an internal-RAM stack (required by the
 *  digital pad hold). Does not return. */
void furi_hal_power_sleep_start_deep_sleep(bool digital_hold);

#if CONFIG_PM_ENABLE
/** Program the light-sleep pad states (Auto Light Sleep enabled). */
void furi_hal_power_sleep_apply_light_sleep_pins(void);

/** Put the same pads back to the IDF default (isolated, no pull). */
void furi_hal_power_sleep_reset_light_sleep_pins(void);
#endif

#ifdef __cplusplus
}
#endif
