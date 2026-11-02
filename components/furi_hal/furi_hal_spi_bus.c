#include "furi_hal_spi_bus.h"

#include <freertos/FreeRTOS.h>

static FuriMutex* furi_hal_spi_bus_mutex = NULL;
/* Tasks currently blocked in furi_hal_spi_bus_lock(). */
static volatile uint32_t furi_hal_spi_bus_waiters = 0;

/* Ticks a radio gives way while others are queued for the bus, at most. */
#define FURI_HAL_SPI_BUS_YIELD_TICKS_MAX 4

void furi_hal_spi_bus_init(void) {
    /* Every lock call goes through here, so two tasks can race on the very first one:
     * with a plain check-then-assign each would end up locking its own mutex and the
     * two would not exclude each other. Install the mutex with a compare-and-set. */
    static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;
    if(furi_hal_spi_bus_mutex) return;

    FuriMutex* candidate = furi_mutex_alloc(FuriMutexTypeRecursive);
    furi_check(candidate);
    portENTER_CRITICAL(&init_lock);
    if(!furi_hal_spi_bus_mutex) furi_hal_spi_bus_mutex = candidate;
    FuriMutex* installed = furi_hal_spi_bus_mutex;
    portEXIT_CRITICAL(&init_lock);
    if(installed != candidate) furi_mutex_free(candidate);
}

void furi_hal_spi_bus_lock(void) {
    furi_hal_spi_bus_init();
    __atomic_add_fetch(&furi_hal_spi_bus_waiters, 1, __ATOMIC_RELAXED);
    furi_check(furi_mutex_acquire(furi_hal_spi_bus_mutex, FuriWaitForever) == FuriStatusOk);
    __atomic_sub_fetch(&furi_hal_spi_bus_waiters, 1, __ATOMIC_RELAXED);
}

void furi_hal_spi_bus_lock_yielding(void) {
    furi_hal_spi_bus_init();
    /* A FreeRTOS mutex is not handed over on release: a radio thread doing
     * back-to-back register transactions re-takes it before a woken waiter
     * (often the GUI on the other core) gets to run, starving display commits
     * ("ViewPort lockup", frozen UI). Step aside briefly while someone else
     * is queued, unless we already own the lock (recursive use). */
    if(!FURI_IS_ISR() && furi_kernel_is_running() &&
       furi_mutex_get_owner(furi_hal_spi_bus_mutex) != furi_thread_get_current_id()) {
        for(uint32_t i = 0; i < FURI_HAL_SPI_BUS_YIELD_TICKS_MAX &&
                            __atomic_load_n(&furi_hal_spi_bus_waiters, __ATOMIC_RELAXED);
            i++) {
            furi_delay_tick(1);
        }
    }
    furi_hal_spi_bus_lock();
}

void furi_hal_spi_bus_unlock(void) {
    furi_check(furi_hal_spi_bus_mutex);
    furi_check(furi_mutex_release(furi_hal_spi_bus_mutex) == FuriStatusOk);
}
