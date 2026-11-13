#ifndef _SPI_LIB_H_
#define _SPI_LIB_H_

// ESP32 port note:
// The original (STM32) Spi_lib drove the Flipper's external SPI bus through the
// LL_SPI register API (furi_hal_spi_bus_r + furi_hal_spi_preset_1edge_low_8m and
// a hand-rolled FuriHalSpiBusHandle callback). None of that exists on the ESP32
// port — this firmware exposes ready-made bus *handles* instead. Spi_lib is
// reimplemented (Spi_lib.c) on top of furi_hal_spi_bus_handle_external, which
// drives the same external-header pins the original app used (pa4/pb3/pa7/pa6).
// The public API below is unchanged, so mcp_can_2515.c keeps working verbatim.

#include <furi_hal.h>
#include <furi_hal_spi.h>
#include <furi_hal_spi_types.h>

#define TIMEOUT_SPI 100

// FUNCTIONS
FuriHalSpiBusHandle* spi_alloc();
bool spi_send(FuriHalSpiBusHandle* spi, uint8_t* buffer, size_t len);
bool spi_send_and_read(
    FuriHalSpiBusHandle* spi,
    uint8_t* action_address,
    size_t addr_len,
    uint8_t* data_read,
    size_t read_len);

#endif
