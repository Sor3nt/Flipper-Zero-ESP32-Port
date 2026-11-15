#include "furi_hal_spi_bus.h"

static FuriMutex* furi_hal_spi_bus_mutex = NULL;

void furi_hal_spi_bus_init(void) {
    if(!furi_hal_spi_bus_mutex) {
        furi_hal_spi_bus_mutex = furi_mutex_alloc(FuriMutexTypeRecursive);
        furi_check(furi_hal_spi_bus_mutex);
    }
}

void furi_hal_spi_bus_lock(void) {
    furi_hal_spi_bus_init();
    furi_check(furi_mutex_acquire(furi_hal_spi_bus_mutex, FuriWaitForever) == FuriStatusOk);
}

bool furi_hal_spi_bus_lock_timeout(uint32_t timeout_ms) {
    furi_hal_spi_bus_init();
    return furi_mutex_acquire(furi_hal_spi_bus_mutex, pdMS_TO_TICKS(timeout_ms)) ==
           FuriStatusOk;
}

void furi_hal_spi_bus_unlock(void) {
    furi_check(furi_hal_spi_bus_mutex);
    furi_check(furi_mutex_release(furi_hal_spi_bus_mutex) == FuriStatusOk);
}
