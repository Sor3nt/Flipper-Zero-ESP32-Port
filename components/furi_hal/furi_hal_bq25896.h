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

/** True if the charger is currently powered from VBUS (USB attached). */
bool furi_hal_bq25896_is_vbus_present(void);

/** Same as furi_hal_bq25896_is_vbus_present() (single read of REG0B), but tells
 *  a failed I2C read apart from "no VBUS".
 *  @return false if the state could not be read (then *present is untouched) */
bool furi_hal_bq25896_try_vbus_present(bool* present);

/** Enter ship mode (BATFET off): physically disconnect the battery for a real
 *  power-off. Only effective on battery — with USB attached SYS stays powered. */
void furi_hal_bq25896_poweroff(void);

/* ---- Boards with BOARD_HAS_BQ25896_SHIP_MODE only (Paradoxt) ----
 * Not implemented on other boards; only called from code guarded by that
 * macro. */

/** VBUS_GD (REG11 bit 7), up to 3 read attempts.
 *  @return false if the state could not be read (then *present is untouched) */
bool furi_hal_bq25896_get_vbus_present(bool* present);

/** Charger set up to keep charging on its own while the MCU deep-sleeps:
 *  EN_HIZ=0, CHG_CONFIG=1, OTG off, watchdog off, ADC off; verified by
 *  read-back (3 attempts). */
bool furi_hal_bq25896_prepare_for_sleep(void);

/** Verified ship mode: refuses with VBUS_GD=1; charger/OTG and watchdog off,
 *  then BATFET_DIS with BATFET_DLY=0 and BATFET_RST_EN=0 (3 attempts). Does
 *  not return when the battery is cut. Returns false (charger state
 *  restored) if the system is still powered afterwards. */
bool furi_hal_bq25896_ship_mode(void);

#ifdef __cplusplus
}
#endif
