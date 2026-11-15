/**
 * @file furi_hal_ina219.h
 * INA219 current/voltage/power monitor driver (I2C).
 *
 * Used by boards that have no dedicated fuel gauge (e.g. Fatal Core, which
 * uses a TP4056 charger + INA219 on the battery rail). The API intentionally
 * mirrors furi_hal_bq27220.h so furi_hal_power.c can pick whichever gauge is
 * populated at runtime.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the INA219 on the shared Qwiic/power I2C bus.
 *  @return true if the device answered and was configured */
bool furi_hal_ina219_init(void);

/** True when the INA219 answered on the bus and calibration was written. */
bool furi_hal_ina219_is_present(void);

/** Bus (battery) voltage in millivolts. */
uint16_t furi_hal_ina219_get_voltage_mv(void);

/** Instantaneous battery current in mA.
 *  Positive = current flowing into the battery (charging),
 *  negative = discharge. Sign depends on how the shunt is wired. */
int16_t furi_hal_ina219_get_current_ma(void);

/** Instantaneous power in milliwatts (always non-negative). */
uint32_t furi_hal_ina219_get_power_mw(void);

/** Shunt voltage in microvolts (signed). */
int32_t furi_hal_ina219_get_shunt_uv(void);

#ifdef __cplusplus
}
#endif