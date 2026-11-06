/**
 * @file furi_hal_i2c.h
 * @brief I2C HAL stub for ESP32 port.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FuriHalI2cBus FuriHalI2cBus;
typedef struct FuriHalI2cBusHandle FuriHalI2cBusHandle;

typedef enum {
    FuriHalI2cBeginStart,
    FuriHalI2cBeginRestart,
    FuriHalI2cBeginResume,
} FuriHalI2cBegin;

typedef enum {
    FuriHalI2cEndStop,
    FuriHalI2cEndAwaitRestart,
    FuriHalI2cEndPause,
} FuriHalI2cEnd;

extern const FuriHalI2cBusHandle furi_hal_i2c_handle_external;

void furi_hal_i2c_acquire(const FuriHalI2cBusHandle* handle);
void furi_hal_i2c_release(const FuriHalI2cBusHandle* handle);

bool furi_hal_i2c_tx(
    const FuriHalI2cBusHandle* handle,
    uint8_t address,
    const uint8_t* data,
    size_t size,
    uint32_t timeout);

bool furi_hal_i2c_rx(
    const FuriHalI2cBusHandle* handle,
    uint8_t address,
    uint8_t* data,
    size_t size,
    uint32_t timeout);

bool furi_hal_i2c_trx(
    const FuriHalI2cBusHandle* handle,
    uint8_t address,
    const uint8_t* tx_data,
    size_t tx_size,
    uint8_t* rx_data,
    size_t rx_size,
    uint32_t timeout);

bool furi_hal_i2c_is_device_ready(
    const FuriHalI2cBusHandle* handle,
    uint8_t i2c_addr,
    uint32_t timeout);

bool furi_hal_i2c_read_reg_8(
    const FuriHalI2cBusHandle* handle,
    uint8_t i2c_addr,
    uint8_t reg_addr,
    uint8_t* data,
    uint32_t timeout);

bool furi_hal_i2c_read_reg_16(
    const FuriHalI2cBusHandle* handle,
    uint8_t i2c_addr,
    uint8_t reg_addr,
    uint16_t* data,
    uint32_t timeout);

bool furi_hal_i2c_read_mem(
    const FuriHalI2cBusHandle* handle,
    uint8_t i2c_addr,
    uint8_t mem_addr,
    uint8_t* data,
    size_t len,
    uint32_t timeout);

bool furi_hal_i2c_write_reg_8(
    const FuriHalI2cBusHandle* handle,
    uint8_t i2c_addr,
    uint8_t reg_addr,
    uint8_t data,
    uint32_t timeout);

bool furi_hal_i2c_write_reg_16(
    const FuriHalI2cBusHandle* handle,
    uint8_t i2c_addr,
    uint8_t reg_addr,
    uint16_t data,
    uint32_t timeout);

bool furi_hal_i2c_write_mem(
    const FuriHalI2cBusHandle* handle,
    uint8_t i2c_addr,
    uint8_t mem_addr,
    const uint8_t* data,
    size_t len,
    uint32_t timeout);

/* ---- Board description and address policy (ESP32 port) ----
 *
 * The external bus is the board's Qwiic bus (or, on boards without one, the NFC bus).
 * Its pins and port come from the board header inside the firmware: an app never
 * hard-codes them, so one FAP works on every board. All transfers above use 8-bit
 * addresses (7-bit address << 1) like the stock Flipper API; the functions below use
 * plain 7-bit addresses. */

typedef enum {
    FuriHalI2cAccessFree, /**< no firmware driver uses this address */
    FuriHalI2cAccessReadOnly, /**< a firmware driver owns it: reads are fine, writes are refused */
    FuriHalI2cAccessExclusive, /**< a stateful firmware driver owns it: reads and writes are refused */
} FuriHalI2cAccess;

/** True if this board has an external I2C bus an app may use. */
bool furi_hal_i2c_external_available(void);

/** SDA / SCL GPIO numbers of the external bus (UINT8_MAX when there is none). */
bool furi_hal_i2c_external_get_pins(uint8_t* sda, uint8_t* scl);

/** What the firmware itself does with a 7-bit address on this board. The HAL
 *  enforces it in tx / rx / trx (so also in the register helpers). */
FuriHalI2cAccess furi_hal_i2c_get_access(uint8_t addr7);

/** Name of the firmware device on a 7-bit address ("BQ25896 charger"), or NULL. */
const char* furi_hal_i2c_get_owner(uint8_t addr7);

/** Two free header pads the board offers for passively sniffing an I2C bus, on boards
 *  that have any (never the bus pins themselves: taking those off the I2C peripheral
 *  would starve the fuel gauge, the charger and NFC). The caller must lease both with
 *  furi_hal_gpio_reserve() before using them. UINT8_MAX / false when there are none. */
bool furi_hal_i2c_get_sniffer_pins(uint8_t* sda, uint8_t* scl);

#ifdef __cplusplus
}
#endif
