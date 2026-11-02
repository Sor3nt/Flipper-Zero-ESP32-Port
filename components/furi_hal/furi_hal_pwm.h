/**
 * @file furi_hal_pwm.h
 * @brief PWM HAL for the ESP32 port (LEDC square wave on the external GPIO header).
 *
 * Outputs are the board's external header pins (BOARD_PIN_EXT_GPIO_1..5), never the
 * STM32 timer pins of upstream Flipper: on this port gpio_ext_pa4/pa7 are the CC1101
 * SPI pins and must not be driven as PWM.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/** Lowest / highest frequency the LEDC timers can produce from the 80 MHz APB clock
 *  (14-bit divider limit at the low end, 1-bit duty resolution at the high end). */
#define FURI_HAL_PWM_FREQ_MIN 5UL
#define FURI_HAL_PWM_FREQ_MAX 40000000UL

typedef enum {
    FuriHalPwmOutputIdNone,
    /** STM32 legacy ids, kept so old sources still compile. Unsupported here. */
    FuriHalPwmOutputIdTim1PA7,
    FuriHalPwmOutputIdLptim2PA4,
    /** External header pins BOARD_PIN_EXT_GPIO_1..5 (absent on boards without them). */
    FuriHalPwmOutputIdExtGpio1,
    FuriHalPwmOutputIdExtGpio2,
    FuriHalPwmOutputIdExtGpio3,
    FuriHalPwmOutputIdExtGpio4,
    FuriHalPwmOutputIdExtGpio5,
} FuriHalPwmOutputId;

/**
 * Start PWM on a pin. Does nothing (is_running stays false) when the pin does not
 * exist on this board, is reserved by another user (e.g. IR external TX), is already
 * driven as an output by someone else, or both LEDC slots are taken.
 *
 * @param channel output id
 * @param freq    Hz, clamped to FURI_HAL_PWM_FREQ_MIN..FURI_HAL_PWM_FREQ_MAX
 * @param duty    percent 0..100
 */
void furi_hal_pwm_start(FuriHalPwmOutputId channel, uint32_t freq, uint8_t duty);

/** Stop PWM and return the pin to idle (input, pulled down). */
void furi_hal_pwm_stop(FuriHalPwmOutputId channel);

/** Change frequency/duty of a running output. */
void furi_hal_pwm_set_params(FuriHalPwmOutputId channel, uint32_t freq, uint8_t duty);

bool furi_hal_pwm_is_running(FuriHalPwmOutputId channel);

/**
 * Set the frequency and the high time of a running output, for signals defined by pulse
 * width rather than duty (RC servo / ESC: 50 Hz, 1000..2000 us). The width is rounded to
 * the LEDC duty step, 1/(freq * 2^res): about 1.2 us at 50 Hz (14-bit). A width of one
 * period or more is full-high. Changing only the width keeps the timer phase, and the new
 * width starts with the next period, so no pulse is cut short or stretched.
 *
 * @param channel  output id, already started with furi_hal_pwm_start()
 * @param freq     Hz, clamped to FURI_HAL_PWM_FREQ_MIN..FURI_HAL_PWM_FREQ_MAX
 * @param pulse_us high time in microseconds
 * @return true when applied; false when the output is not running or LEDC refused
 */
bool furi_hal_pwm_set_pulse(FuriHalPwmOutputId channel, uint32_t freq, uint32_t pulse_us);

#ifdef __cplusplus
}
#endif
