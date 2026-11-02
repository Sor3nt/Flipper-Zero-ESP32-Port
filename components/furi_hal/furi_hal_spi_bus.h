#pragma once

#include <furi.h>

#ifdef __cplusplus
extern "C" {
#endif

void furi_hal_spi_bus_init(void);
void furi_hal_spi_bus_lock(void);
/** Like furi_hal_spi_bus_lock(), but first gives way (a few ticks at most)
 * while other tasks are waiting for the bus. For high-rate radio transactions
 * so they cannot starve the display/SD. Not for use from ISR. */
void furi_hal_spi_bus_lock_yielding(void);
void furi_hal_spi_bus_unlock(void);

#ifdef __cplusplus
}
#endif
