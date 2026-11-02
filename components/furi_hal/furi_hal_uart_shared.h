#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    FuriHalUartSharedAcquireAcquired,
    FuriHalUartSharedAcquireBusy,
    FuriHalUartSharedAcquireError,
} FuriHalUartSharedAcquireResult;

/* Serializes shared UART users; normal clients preempt background listeners. */
void furi_hal_uart_shared_init(void);
/* timeout_ms is in milliseconds; every successful acquire must be released. */
FuriHalUartSharedAcquireResult furi_hal_uart_shared_acquire(uint32_t timeout_ms);
/* Background clients yield their UART lease while a normal client is waiting or active. */
FuriHalUartSharedAcquireResult furi_hal_uart_shared_acquire_background(uint32_t timeout_ms);
bool furi_hal_uart_shared_preemption_requested(void);
/* Leaves shared pins in a pulled idle state before releasing ownership. */
bool furi_hal_uart_shared_release(void);
