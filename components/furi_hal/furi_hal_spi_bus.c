#include "furi_hal_spi_bus.h"

#include <string.h>

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

/* QFDIAG: diagnostic only — who holds the bus lock right now. */
void furi_hal_spi_bus_owner_name(char* out, size_t len) {
    if(!out || !len) return;
    const char* name = "-";
    if(furi_hal_spi_bus_mutex) {
        FuriThreadId owner = furi_mutex_get_owner(furi_hal_spi_bus_mutex);
        if(!owner) {
            name = "none";
        } else {
            const char* n = furi_thread_get_name(owner);
            name = n ? n : "?";
        }
    }
    strlcpy(out, name, len);
}
