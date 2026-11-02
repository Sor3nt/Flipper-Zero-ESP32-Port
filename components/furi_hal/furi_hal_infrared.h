/**
 * @file furi_hal_infrared.h
 * INFRARED HAL API
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void furi_hal_infrared_init(void);

#define INFRARED_MAX_FREQUENCY 1000000
#define INFRARED_MIN_FREQUENCY 10000

typedef enum {
    FuriHalInfraredTxPinInternal,
    FuriHalInfraredTxPinExtPA7,
    FuriHalInfraredTxPinMax,
} FuriHalInfraredTxPin;

typedef enum {
    FuriHalInfraredTxGetDataStateOk, /**< New data obtained */
    FuriHalInfraredTxGetDataStateDone, /**< New data obtained, and this is end of package */
    FuriHalInfraredTxGetDataStateLastDone, /**< New data obtained, and this is end of package and no more data available */
} FuriHalInfraredTxGetDataState;

/** Callback type for providing data to INFRARED TX.
 *
 * The ESP32 implementation invokes this from the dedicated TX task, despite
 * the legacy ISR suffix in the type name. The callback must not block on
 * unrelated work and must remain safe while TX is active.
 */
typedef FuriHalInfraredTxGetDataState (
    *FuriHalInfraredTxGetDataISRCallback)(void* context, uint32_t* duration, bool* level);

/** Callback type called every time a signal packet is sent.
 *
 * Actually, it means there are 2 timings left to send for this signal, which is
 * almost end. Don't use this callback to stop transmission, as far as there are
 * next signal is charged for transmission by the TX task.
 */
typedef void (*FuriHalInfraredTxSignalSentISRCallback)(void* context);

/** Callback called when TX packet assembly exceeds the symbol buffer. */
typedef void (*FuriHalInfraredTxOverflowISRCallback)(void* context);

/** Callback called when TX cannot complete. */
typedef void (*FuriHalInfraredTxErrorCallback)(void* context);

/** Signature of callback function for receiving continuous INFRARED rx signal.
 *
 * The callback runs from an IRAM-safe ISR and must be IRAM-safe itself. Any
 * state it accesses must reside in internal RAM while flash cache is disabled.
 *
 * @param[in] ctx       context to pass to callback
 * @param[in] level     level of input INFRARED rx signal
 * @param[in] duration  duration of continuous rx signal level in us
 * @return              true if a higher-priority task may need to run
 */
typedef bool (*FuriHalInfraredRxCaptureCallback)(void* ctx, bool level, uint32_t duration);

/** Signature of callback function for reaching silence timeout on INFRARED port.
 *
 * The callback runs from an IRAM-safe ISR and must be IRAM-safe itself. Any
 * state it accesses must reside in internal RAM while flash cache is disabled.
 *
 * @param[in] ctx  context to pass to callback
 * @return         true if a higher-priority task may need to run
 */
typedef bool (*FuriHalInfraredRxTimeoutCallback)(void* ctx);

/** Callback for an idle-terminated RX burst. Runs from an IRAM-safe ISR. */
typedef bool (*FuriHalInfraredRxFrameEndCallback)(void* ctx, uint32_t gap_us);

/** Callback called when RX rearm fails. Runs from the RX rearm task context. */
typedef void (*FuriHalInfraredRxErrorCallback)(void* ctx);

/** Initialize INFRARED RX timer to receive interrupts.
 *
 * The caller must first reserve the peripheral with furi_hal_infrared_claim_rx().
 *
 * It provides interrupts for every RX-signal edge changing with its duration.
 *
 * @return true on success. If false is returned, the reservation is not
 *         released internally; the caller must call
 *         furi_hal_infrared_release_claim().
 */
bool furi_hal_infrared_async_rx_start(void);

/** Atomically reserve the infrared peripheral for RX configuration. */
bool furi_hal_infrared_claim_rx(void);

/** Atomically reserve the infrared peripheral for TX configuration. */
bool furi_hal_infrared_claim_tx(void);

/** Release a reservation that has not been started. */
void furi_hal_infrared_release_claim(void);

/** Deinitialize INFRARED RX interrupt.
 */
void furi_hal_infrared_async_rx_stop(void);

/** Setup hal for receiving silence timeout.
 *
 * Should be used with 'furi_hal_infrared_timeout_irq_set_callback()'.
 *
 * @param[in]  timeout_us  time to wait for silence on INFRARED port before
 *                         generating IRQ.
 */
void furi_hal_infrared_async_rx_set_timeout(uint32_t timeout_us);

/** Setup callback for previously initialized INFRARED RX interrupt.
 *
 * @param[in]  callback  callback to call when RX signal edge changing occurs
 * @param[in]  ctx       context for callback
 */
void furi_hal_infrared_async_rx_set_capture_isr_callback(
    FuriHalInfraredRxCaptureCallback callback,
    void* ctx);

/** Setup callback for reaching silence timeout on INFRARED port.
 *
 * Should setup hal with 'furi_hal_infrared_setup_rx_timeout_irq()' first.
 *
 * @param[in]  callback  callback for silence timeout
 * @param[in]  ctx       context to pass to callback
 */
void furi_hal_infrared_async_rx_set_timeout_isr_callback(
    FuriHalInfraredRxTimeoutCallback callback,
    void* ctx);

void furi_hal_infrared_async_rx_set_frame_end_isr_callback(
    FuriHalInfraredRxFrameEndCallback callback,
    void* ctx);

void furi_hal_infrared_async_rx_set_error_callback(
    FuriHalInfraredRxErrorCallback callback,
    void* ctx);

/** Check if INFRARED is in use now.
 *
 * @return     true if INFRARED is busy, false otherwise.
 */
bool furi_hal_infrared_is_busy(void);

/** Return whether the selected board provides infrared hardware. */
bool furi_hal_infrared_is_available(void);

/** Set callback providing new data.
 *
 * This function has to be called before furi_hal_infrared_async_tx_start().
 *
 * @param[in]  callback  function to provide new data
 * @param[in]  context   context for callback
 */
void furi_hal_infrared_async_tx_set_data_isr_callback(
    FuriHalInfraredTxGetDataISRCallback callback,
    void* context);

/** Start IR asynchronous transmission.
 *
 * The caller must first reserve the peripheral with furi_hal_infrared_claim_tx().
 * If false is returned, the reservation is not released internally; the
 * caller must call furi_hal_infrared_release_claim().
 *
 * It can be stopped by 2 reasons:
 * 1. implicit call for furi_hal_infrared_async_tx_stop()
 * 2. callback can provide FuriHalInfraredTxGetDataStateLastDone response which
 *    means no more data available for transmission.
 *
 * Any func (furi_hal_infrared_async_tx_stop() or
 * furi_hal_infrared_async_tx_wait_termination()) has to be called to wait end of
 * transmission and free resources.
 *
 * @param[in]  freq        frequency for PWM
 * @param[in]  duty_cycle  duty cycle for PWM
 */
bool furi_hal_infrared_async_tx_start(uint32_t freq, float duty_cycle);

/** Stop IR asynchronous transmission and free resources.
 *
 * Transmission will stop as soon as transmission reaches end of package
 * (FuriHalInfraredTxGetDataStateDone or FuriHalInfraredTxGetDataStateLastDone).
 */
void furi_hal_infrared_async_tx_stop(void);

/** Wait for end of IR asynchronous transmission and free resources.
 *
 * Transmission will stop as soon as transmission reaches end of transmission
 * (FuriHalInfraredTxGetDataStateLastDone).
 */
void furi_hal_infrared_async_tx_wait_termination(void);

/** Set callback for end of signal transmission
 *
 * @param[in]  callback  function to call when signal is sent
 * @param[in]  context   context for callback
 */
void furi_hal_infrared_async_tx_set_signal_sent_isr_callback(
    FuriHalInfraredTxSignalSentISRCallback callback,
    void* context);

/** Set callback for a TX symbol-buffer overflow. */
void furi_hal_infrared_async_tx_set_overflow_isr_callback(
    FuriHalInfraredTxOverflowISRCallback callback,
    void* context);

void furi_hal_infrared_async_tx_set_error_callback(
    FuriHalInfraredTxErrorCallback callback,
    void* context);

/** Detect which pin has an external IR module connected.
 *
 * @return FuriHalInfraredTxPinInternal (always, on ESP32).
 */
FuriHalInfraredTxPin furi_hal_infrared_detect_tx_output(void);

/** Set which pin will be used to transmit infrared signals.
 *
 * FuriHalInfraredTxPinExtPA7 selects the board's external IR output
 * (BOARD_PIN_IR_TX_EXT, GPIO4 on Paradoxt). That pin is only driven while a
 * transmission runs and is released (input, pull-down) afterwards. Takes effect
 * from the next transmission; unsupported outputs fall back to internal.
 *
 * @param[in]   tx_pin  pin to be used for signal transmission.
 */
void furi_hal_infrared_set_tx_output(FuriHalInfraredTxPin tx_pin);

/** @return output used by the next transmission */
FuriHalInfraredTxPin furi_hal_infrared_get_tx_output(void);

/** @return true if this board can transmit on the given output */
bool furi_hal_infrared_is_tx_output_supported(FuriHalInfraredTxPin tx_pin);

/** Check that the selected output can be driven right now.
 *
 * @return false if the external pin is reserved by another HAL user or is
 *         configured as an output by someone else (CLI, RPC, Expansion, script)
 */
bool furi_hal_infrared_is_tx_output_free(void);

#ifdef __cplusplus
}
#endif