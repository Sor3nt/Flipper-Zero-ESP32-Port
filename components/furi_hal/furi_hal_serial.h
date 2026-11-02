/**
 * @file furi_hal_serial.h
 * @brief Serial UART HAL for the ESP32 port.
 *
 * API-compatible subset of upstream Flipper's furi_hal_serial, so apps written against it
 * (WiFi Marauder companion, js_serial, uart_echo) run unchanged. Backed by the shared
 * external UART (furi_hal_uart_shared, GPIO43/44) and the ESP-IDF UART driver.
 *
 * The async RX callback contract matches upstream: async_rx_start() arranges for the
 * callback to run once per received byte with FuriHalSerialRxEventData; the callback then
 * calls furi_hal_serial_async_rx() to read that byte. On this port the callback runs in a
 * dedicated reader thread (not an ISR), which is strictly safer for the stream-buffer and
 * thread-flag calls apps make inside it.
 */
#pragma once

#include "furi_hal_serial_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Receive callback. Runs in the reader thread on this port. */
typedef void (*FuriHalSerialAsyncRxCallback)(
    FuriHalSerialHandle* handle,
    FuriHalSerialRxEvent event,
    void* context);

/** Initialize UART at the given baud rate (claims the driver + holds light sleep off). */
void furi_hal_serial_init(FuriHalSerialHandle* handle, uint32_t baud_rate);

/** Deinitialize UART (releases the driver). Safe to re-init afterwards. */
void furi_hal_serial_deinit(FuriHalSerialHandle* handle);

/** Suspend UART (kept for API compatibility; no-op beyond stopping RX on this port). */
void furi_hal_serial_suspend(FuriHalSerialHandle* handle);

/** Resume UART (API compatibility). */
void furi_hal_serial_resume(FuriHalSerialHandle* handle);

/** Change baud rate on an initialized handle. */
void furi_hal_serial_set_br(FuriHalSerialHandle* handle, uint32_t baud_rate);

/** Configure framing (data bits, parity, stop bits). */
void furi_hal_serial_configure_framing(
    FuriHalSerialHandle* handle,
    FuriHalSerialDataBits data_bits,
    FuriHalSerialParity parity,
    FuriHalSerialStopBits stop_bits);

/** True if the baud rate can be programmed on this port. */
bool furi_hal_serial_is_baud_rate_supported(FuriHalSerialHandle* handle, uint32_t baud_rate);

/** Transmit a buffer (blocking until queued). */
void furi_hal_serial_tx(FuriHalSerialHandle* handle, const uint8_t* buffer, size_t buffer_size);

/** Block until all queued TX bytes have left the UART. */
void furi_hal_serial_tx_wait_complete(FuriHalSerialHandle* handle);

/** Start async RX: callback runs per received byte (and, if report_errors, on error events). */
void furi_hal_serial_async_rx_start(
    FuriHalSerialHandle* handle,
    FuriHalSerialAsyncRxCallback callback,
    void* context,
    bool report_errors);

/** Stop async RX and join the reader thread. */
void furi_hal_serial_async_rx_stop(FuriHalSerialHandle* handle);

/** Read the byte that triggered the current Data callback. Valid only inside the callback. */
uint8_t furi_hal_serial_async_rx(FuriHalSerialHandle* handle);

/** True while more bytes from the current burst are waiting to be delivered. */
bool furi_hal_serial_async_rx_available(FuriHalSerialHandle* handle);

#ifdef __cplusplus
}
#endif
