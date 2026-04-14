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

#ifdef __cplusplus
}
#endif
