/**
 * @file furi_hal_bq25896.h
 * BQ25896 charger driver (I2C) — ported from STM32 Flipper firmware
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool furi_hal_bq25896_init(void);
bool furi_hal_bq25896_is_present(void);

bool furi_hal_bq25896_is_charging(void);
bool furi_hal_bq25896_is_charging_done(void);

void furi_hal_bq25896_enable_charging(void);
void furi_hal_bq25896_disable_charging(void);

uint16_t furi_hal_bq25896_get_vbus_voltage_mv(void);
uint16_t furi_hal_bq25896_get_vbat_voltage_mv(void);
uint16_t furi_hal_bq25896_get_vbat_current_ma(void);
int32_t furi_hal_bq25896_get_temperature_mc(void); /* milli-celsius */

uint16_t furi_hal_bq25896_get_vreg_voltage_mv(void);
void furi_hal_bq25896_set_vreg_voltage_mv(uint16_t vreg_mv);

void furi_hal_bq25896_enable_otg(void);
void furi_hal_bq25896_disable_otg(void);
bool furi_hal_bq25896_is_otg_enabled(void);

#ifdef __cplusplus
}
#endif
