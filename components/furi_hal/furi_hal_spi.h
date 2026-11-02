#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <furi_hal_spi_types.h>

#ifdef __cplusplus
extern "C" {
#endif

void furi_hal_spi_init(void);

void furi_hal_spi_bus_handle_init(const FuriHalSpiBusHandle* handle);
void furi_hal_spi_bus_handle_deinit(const FuriHalSpiBusHandle* handle);

/** Re-point a handle at another chip-select pad (same bus, MOSI/MISO/SCK untouched).
 *  The old SPI device is removed so its CS pad is released; the new one is
 *  created on the next furi_hal_spi_acquire(). Returns false (nothing changed)
 *  while the handle is acquired. */
bool furi_hal_spi_bus_handle_set_cs(const FuriHalSpiBusHandle* handle, const GpioPin* cs);

void furi_hal_spi_acquire(const FuriHalSpiBusHandle* handle);
void furi_hal_spi_release(const FuriHalSpiBusHandle* handle);

bool furi_hal_spi_bus_rx(
    const FuriHalSpiBusHandle* handle,
    uint8_t* buffer,
    size_t size,
    uint32_t timeout);

bool furi_hal_spi_bus_tx(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* buffer,
    size_t size,
    uint32_t timeout);

bool furi_hal_spi_bus_trx(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* tx_buffer,
    uint8_t* rx_buffer,
    size_t size,
    uint32_t timeout);

bool furi_hal_spi_bus_trx_dma(
    const FuriHalSpiBusHandle* handle,
    uint8_t* tx_buffer,
    uint8_t* rx_buffer,
    size_t size,
    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
