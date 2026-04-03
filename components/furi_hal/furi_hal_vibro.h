/**
 * @file furi_hal_vibro.h
 * Vibro HAL API (ESP32 stub)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize vibro
 */
void furi_hal_vibro_init(void);

/** Turn on/off vibro
 *
 * @param[in]  value  new state
 */
void furi_hal_vibro_on(bool value);

#ifdef __cplusplus
}
#endif
