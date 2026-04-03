/**
 * @file furi_hal_bq27220.h
 * BQ27220 fuel gauge driver (I2C)
 * API mirrors STM32 Flipper firmware (lib/drivers/bq27220.h)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize BQ27220 on the Qwiic/Grove I2C bus.
 *  @return true if the device was found and responded */
bool furi_hal_bq27220_init(void);

/** Check if the BQ27220 is present and responding */
bool furi_hal_bq27220_is_present(void);

/** Get battery charge percentage (0–100) — StateOfCharge register */
uint8_t furi_hal_bq27220_get_charge_pct(void);

/** Get state of health percentage (0–100) */
uint8_t furi_hal_bq27220_get_health_pct(void);

/** Get whether the battery is currently charging (DSG bit = 0) */
bool furi_hal_bq27220_is_charging(void);

/** Get battery voltage in mV */
uint16_t furi_hal_bq27220_get_voltage_mv(void);

/** Get instantaneous current in mA (signed: negative = discharging) */
int16_t furi_hal_bq27220_get_current_ma(void);

/** Get battery temperature in 0.1°K units (convert: (raw - 2731) / 10.0 = °C) */
uint16_t furi_hal_bq27220_get_temperature_raw(void);

/** Get remaining capacity in mAh */
uint16_t furi_hal_bq27220_get_remaining_capacity_mah(void);

/** Get full charge capacity in mAh */
uint16_t furi_hal_bq27220_get_full_charge_capacity_mah(void);

/** Get design capacity in mAh */
uint16_t furi_hal_bq27220_get_design_capacity_mah(void);

#ifdef __cplusplus
}
#endif
