#include "Spi_lib.h"

// ESP32 port of the tiny SPI shim mcp_can_2515.c sits on. See Spi_lib.h for why
// the STM32 LL_SPI implementation was replaced. Here we borrow the firmware's
// external-header bus handle (furi_hal_spi_bus_handle_external) — same pins the
// original app used — and expose the same three functions.

//  Alloc the SPI handle used by the MCP2515 driver.
FuriHalSpiBusHandle* spi_alloc() {
    // free_mcp2515() calls free() on this pointer, so it MUST be heap-allocated
    // (not a pointer to the static firmware handle). Copy the ready-made
    // external-bus handle by value: it carries the correct pins plus a pointer
    // to the shared bus (mutex etc.), so acquire/release still serialize against
    // any other user of that bus.
    FuriHalSpiBusHandle* spi = malloc(sizeof(FuriHalSpiBusHandle));
    if(!spi) return NULL;
    *spi = furi_hal_spi_bus_handle_external;
    return spi;
}

// Send data (write-only transaction).
bool spi_send(FuriHalSpiBusHandle* spi, uint8_t* buffer, size_t len) {
    furi_hal_spi_acquire(spi);
    bool ret = furi_hal_spi_bus_tx(spi, buffer, len, TIMEOUT_SPI);
    furi_hal_spi_release(spi);
    return ret;
}

// Write an address/command, then read the response back.
bool spi_send_and_read(
    FuriHalSpiBusHandle* spi,
    uint8_t* action_address,
    size_t addr_len,
    uint8_t* data_read,
    size_t read_len) {
    furi_hal_spi_acquire(spi);
    bool ret =
        (furi_hal_spi_bus_tx(spi, action_address, addr_len, TIMEOUT_SPI) &&
         furi_hal_spi_bus_rx(spi, data_read, read_len, TIMEOUT_SPI));
    furi_hal_spi_release(spi);
    return ret;
}
