/**
 * @file furi_hal_pwm.h
 * @brief PWM control HAL stub for ESP32 port.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    FuriHalPwmOutputIdNone,
    FuriHalPwmOutputIdTim1PA7,
    FuriHalPwmOutputIdLptim2PA4,
} FuriHalPwmOutputId;

void furi_hal_pwm_start(FuriHalPwmOutputId channel, uint32_t freq, uint8_t duty);
void furi_hal_pwm_stop(FuriHalPwmOutputId channel);
void furi_hal_pwm_set_params(FuriHalPwmOutputId channel, uint32_t freq, uint8_t duty);
bool furi_hal_pwm_is_running(FuriHalPwmOutputId channel);

#ifdef __cplusplus
}
#endif
