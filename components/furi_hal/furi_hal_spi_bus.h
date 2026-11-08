#pragma once

#include <furi.h>

#ifdef __cplusplus
extern "C" {
#endif

void furi_hal_spi_bus_init(void);
void furi_hal_spi_bus_lock(void);
/* Try to take the bus, waiting at most timeout_ms. Returns true on success.
 * Used by the display path so a stuck SPI user (slow SD read, NFC) can never
 * freeze the UI forever: the frame is simply dropped and retried later. */
bool furi_hal_spi_bus_lock_timeout(uint32_t timeout_ms);
void furi_hal_spi_bus_unlock(void);

#ifdef __cplusplus
}
#endif
