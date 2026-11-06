#pragma once

/*
 * ESP32 port glue for I2C Tools CLI.
 *
 * The external I2C bus is a firmware HAL now (components/furi_hal/furi_hal_i2c.c): the stock
 * furi_hal_i2c_* calls, the bus pins of the board, the bus lock and the address policy all live
 * there, so the app contains no GPIO number and no board macro. What the policy says:
 *   - addresses of firmware drivers on that bus (BQ25896 charger, BQ27220 gauge, PN532 NFC,
 *     BW16 bridge, OLED) are named by furi_hal_i2c_get_owner(addr7);
 *   - the HAL itself refuses writes to the power ICs and any access to the stateful devices,
 *     whatever this app asks for (furi_hal_i2c_get_access()).
 * Address-only probes (the scan) are always allowed.
 */

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_i2c.h>

/** Check once at start that this board has an external bus the app may use. */
void i2c_compat_init(void);
void i2c_compat_deinit(void);
bool i2c_compat_available(void);

/** Would the HAL refuse this transfer to `addr7`? (`write` = it modifies the device.) */
bool i2c_compat_refused(uint8_t addr7, bool write);
