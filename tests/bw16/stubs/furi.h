#pragma once
#include <stdint.h>
void furi_delay_ms(uint32_t ms);
void furi_hal_spi_bus_lock(void);
void furi_hal_spi_bus_unlock(void);
