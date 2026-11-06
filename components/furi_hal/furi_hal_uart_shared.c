#include "furi_hal_uart_shared.h"

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_err.h>
#include <furi.h>

#include "boards/board.h"

#if defined(BOARD_HAS_RFID) && BOARD_HAS_RFID && \
    defined(BOARD_PIN_RFID_TX) && defined(BOARD_PIN_RFID_RX) && \
    BOARD_PIN_RFID_TX >= 0 && BOARD_PIN_RFID_TX < 64 && \
    BOARD_PIN_RFID_RX >= 0 && BOARD_PIN_RFID_RX < 64
#define FURI_HAL_UART_SHARED_HAS_RFID_PINS 1
#else
#define FURI_HAL_UART_SHARED_HAS_RFID_PINS 0
#endif

#if FURI_HAL_UART_SHARED_HAS_RFID_PINS && \
    !defined(BOARD_EXTERNAL_UART_SAVE_RESTORE_SHARED_PINS)
#define FURI_HAL_UART_SHARED_PARK_RFID_PINS 1
#else
#define FURI_HAL_UART_SHARED_PARK_RFID_PINS 0
#endif

static FuriSemaphore* uart_shared_semaphore;
static uint16_t uart_shared_priority_waiters;
static bool uart_shared_priority_owner;
static bool uart_shared_preemption_requested;

static void furi_hal_uart_shared_update_preemption_locked(void) {
    uart_shared_preemption_requested =
        uart_shared_priority_waiters > 0 || uart_shared_priority_owner;
}

/* Only boards that wire the shared RFID/expansion pins use this; elsewhere it would be
 * an unused static function. */
#if FURI_HAL_UART_SHARED_PARK_RFID_PINS
static esp_err_t furi_hal_uart_shared_set_idle_pin(gpio_num_t pin) {
    esp_err_t err = gpio_reset_pin(pin);
    if(err != ESP_OK) return err;
    err = gpio_set_direction(pin, GPIO_MODE_INPUT);
    if(err != ESP_OK) return err;
    return gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
}
#endif

/* The API takes milliseconds; the semaphore takes ticks. */
static uint32_t furi_hal_uart_shared_timeout_ticks(uint32_t timeout_ms) {
    return timeout_ms == FuriWaitForever ? FuriWaitForever : furi_ms_to_ticks(timeout_ms);
}

void furi_hal_uart_shared_init(void) {
    furi_check(uart_shared_semaphore == NULL);
    uart_shared_semaphore = furi_semaphore_alloc(1, 1);
    furi_check(uart_shared_semaphore);

#if defined(BOARD_UART0_CONSOLE_DETACH_SHARED_PINS) && \
    BOARD_UART0_CONSOLE_DETACH_SHARED_PINS && \
    FURI_HAL_UART_SHARED_PARK_RFID_PINS
    esp_err_t err = furi_hal_uart_shared_set_idle_pin((gpio_num_t)BOARD_PIN_RFID_TX);
    if(err == ESP_OK) {
        err = furi_hal_uart_shared_set_idle_pin((gpio_num_t)BOARD_PIN_RFID_RX);
    }
    if(err != ESP_OK) {
        FURI_LOG_E(
            "UartShared",
            "Could not detach UART0 from shared pins: %s",
            esp_err_to_name(err));
        furi_check(false);
    }
#endif
}

FuriHalUartSharedAcquireResult furi_hal_uart_shared_acquire(uint32_t timeout_ms) {
    if(!uart_shared_semaphore) return FuriHalUartSharedAcquireBusy;

    {
        FURI_CRITICAL_ENTER();
        if(uart_shared_priority_waiters == UINT16_MAX) {
            FURI_CRITICAL_EXIT();
            return FuriHalUartSharedAcquireError;
        }
        uart_shared_priority_waiters++;
        furi_hal_uart_shared_update_preemption_locked();
        FURI_CRITICAL_EXIT();
    }

    const uint32_t timeout_ticks = furi_hal_uart_shared_timeout_ticks(timeout_ms);
    if(furi_semaphore_acquire(uart_shared_semaphore, timeout_ticks) != FuriStatusOk) {
        FURI_CRITICAL_ENTER();
        uart_shared_priority_waiters--;
        furi_hal_uart_shared_update_preemption_locked();
        FURI_CRITICAL_EXIT();
        return FuriHalUartSharedAcquireBusy;
    }

    FURI_CRITICAL_ENTER();
    uart_shared_priority_waiters--;
    uart_shared_priority_owner = true;
    furi_hal_uart_shared_update_preemption_locked();
    FURI_CRITICAL_EXIT();

#if defined(BOARD_UART0_CONSOLE_TX) && defined(BOARD_UART0_CONSOLE_RX) && \
    defined(BOARD_UART0_SINK_TX) && defined(BOARD_UART0_SINK_RX)
    esp_err_t err = uart_set_pin(
        UART_NUM_0,
        BOARD_UART0_SINK_TX,
        BOARD_UART0_SINK_RX,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE);
    if(err != ESP_OK) {
        FURI_CRITICAL_ENTER();
        uart_shared_priority_owner = false;
        furi_hal_uart_shared_update_preemption_locked();
        FURI_CRITICAL_EXIT();
        furi_check(furi_semaphore_release(uart_shared_semaphore) == FuriStatusOk);
        return FuriHalUartSharedAcquireError;
    }
#endif
    return FuriHalUartSharedAcquireAcquired;
}

FuriHalUartSharedAcquireResult furi_hal_uart_shared_acquire_background(uint32_t timeout_ms) {
    if(!uart_shared_semaphore || furi_hal_uart_shared_preemption_requested()) {
        return FuriHalUartSharedAcquireBusy;
    }
    const uint32_t timeout_ticks = furi_hal_uart_shared_timeout_ticks(timeout_ms);
    if(furi_semaphore_acquire(uart_shared_semaphore, timeout_ticks) != FuriStatusOk) {
        return FuriHalUartSharedAcquireBusy;
    }

    FURI_CRITICAL_ENTER();
    const bool preempt = uart_shared_preemption_requested;
    FURI_CRITICAL_EXIT();
    if(preempt) {
        furi_check(furi_semaphore_release(uart_shared_semaphore) == FuriStatusOk);
        return FuriHalUartSharedAcquireBusy;
    }

#if defined(BOARD_UART0_CONSOLE_TX) && defined(BOARD_UART0_CONSOLE_RX) && \
    defined(BOARD_UART0_SINK_TX) && defined(BOARD_UART0_SINK_RX)
    esp_err_t err = uart_set_pin(
        UART_NUM_0,
        BOARD_UART0_SINK_TX,
        BOARD_UART0_SINK_RX,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE);
    if(err != ESP_OK) {
        furi_check(furi_semaphore_release(uart_shared_semaphore) == FuriStatusOk);
        return FuriHalUartSharedAcquireError;
    }
#endif
    return FuriHalUartSharedAcquireAcquired;
}

bool furi_hal_uart_shared_preemption_requested(void) {
    FURI_CRITICAL_ENTER();
    const bool requested = uart_shared_preemption_requested;
    FURI_CRITICAL_EXIT();
    return requested;
}

bool furi_hal_uart_shared_release(void) {
    furi_assert(uart_shared_semaphore);
#if FURI_HAL_UART_SHARED_PARK_RFID_PINS || \
    (defined(BOARD_UART0_CONSOLE_TX) && defined(BOARD_UART0_CONSOLE_RX) && \
     !(defined(BOARD_UART0_CONSOLE_DETACH_SHARED_PINS) && \
       BOARD_UART0_CONSOLE_DETACH_SHARED_PINS))
    esp_err_t err;
#endif
#if FURI_HAL_UART_SHARED_PARK_RFID_PINS
    err = furi_hal_uart_shared_set_idle_pin((gpio_num_t)BOARD_PIN_RFID_TX);
    if(err != ESP_OK) return false;
    err = furi_hal_uart_shared_set_idle_pin((gpio_num_t)BOARD_PIN_RFID_RX);
    if(err != ESP_OK) return false;
#endif
#if defined(BOARD_UART0_CONSOLE_TX) && defined(BOARD_UART0_CONSOLE_RX) && \
    !(defined(BOARD_UART0_CONSOLE_DETACH_SHARED_PINS) && \
      BOARD_UART0_CONSOLE_DETACH_SHARED_PINS)
    err = uart_set_pin(
        UART_NUM_0,
        BOARD_UART0_CONSOLE_TX,
        BOARD_UART0_CONSOLE_RX,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE);
    if(err != ESP_OK) {
        return false;
    }
#endif

    FURI_CRITICAL_ENTER();
    uart_shared_priority_owner = false;
    furi_hal_uart_shared_update_preemption_locked();
    FURI_CRITICAL_EXIT();
    furi_check(furi_semaphore_release(uart_shared_semaphore) == FuriStatusOk);
    return true;
}
