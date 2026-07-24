/**
 * \file furi_hal_battery_stubs.c
 * \brief Stub implementations for battery and power management functions
 * 
 * These functions are used by furi_hal_power.c but are not available
 * when BQ27220 and BQ25896 drivers are disabled due to deprecated I2C API.
 */

#include <stdint.h>
#include <stdbool.h>

/* BQ27220 stubs (battery gauge) */
bool furi_hal_bq27220_init(void) {
    return false;  /* Battery gauge not available */
}

bool furi_hal_bq27220_is_present(void) {
    return false;
}

bool furi_hal_bq27220_is_charging(void) {
    return false;
}

uint16_t furi_hal_bq27220_get_voltage_mv(void) {
    return 0;
}

uint16_t furi_hal_bq27220_get_current_ma(void) {
    return 0;
}

uint8_t furi_hal_bq27220_get_charge_pct(void) {
    return 0;
}

uint8_t furi_hal_bq27220_get_health_pct(void) {
    return 0;
}

uint16_t furi_hal_bq27220_get_remaining_capacity_mah(void) {
    return 0;
}

uint16_t furi_hal_bq27220_get_full_charge_capacity_mah(void) {
    return 0;
}

int32_t furi_hal_bq27220_get_temperature_raw(void) {
    return 0;
}

/* BQ25896 stubs (charger IC) */
bool furi_hal_bq25896_init(void) {
    return false;  /* Charger IC not available */
}

bool furi_hal_bq25896_is_present(void) {
    return false;
}

bool furi_hal_bq25896_is_charging(void) {
    return false;
}

uint16_t furi_hal_bq25896_get_vbat_voltage_mv(void) {
    return 0;
}

uint16_t furi_hal_bq25896_get_vbus_voltage_mv(void) {
    return 0;
}

uint16_t furi_hal_bq25896_get_vreg_voltage_mv(void) {
    return 0;
}

int16_t furi_hal_bq25896_get_vbat_current_ma(void) {
    return 0;
}

int8_t furi_hal_bq25896_get_temperature_mc(void) {
    return 0;
}
