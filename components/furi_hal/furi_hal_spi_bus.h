#pragma once

#include <furi.h>

#ifdef __cplusplus
extern "C" {
#endif

void furi_hal_spi_bus_init(void);
void furi_hal_spi_bus_lock(void);
void furi_hal_spi_bus_unlock(void);

#ifdef __cplusplus
}
#endif
