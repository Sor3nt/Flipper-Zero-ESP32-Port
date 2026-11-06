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

/** Callback type for providing data to INFRARED DMA TX system. It is called every tim */
typedef FuriHalInfraredTxGetDataState (
    *FuriHalInfraredTxGetDataISRCallback)(void* context, uint32_t* duration, bool* level);

/** Callback type called every time signal is sent by DMA to Timer.
 *
 * Actually, it means there are 2 timings left to send for this signal, which is
 * almost end. Don't use this callback to stop transmission, as far as there are
 * next signal is charged for transmission by DMA.
 */
typedef void (*FuriHalInfraredTxSignalSentISRCallback)(void* context);

/** Signature of callback function for receiving continuous INFRARED rx signal.
 *
 * @param[in] ctx       context to pass to callback
 * @param[in] level     level of input INFRARED rx signal
 * @param[in] duration  duration of continuous rx signal level in us
 */
typedef void (*FuriHalInfraredRxCaptureCallback)(void* ctx, bool level, uint32_t duration);

/** Signature of callback function for reaching silence timeout on INFRARED port.
 *
 * @param[in] ctx  context to pass to callback
 */
typedef void (*FuriHalInfraredRxTimeoutCallback)(void* ctx);

/** Initialize INFRARED RX timer to receive interrupts.
 *
 * It provides interrupts for every RX-signal edge changing with its duration.
 */
void furi_hal_infrared_async_rx_start(void);

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

/** Check if INFRARED is in use now.
 *
 * @return     true if INFRARED is busy, false otherwise.
 */
bool furi_hal_infrared_is_busy(void);

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

/** Claim the infrared peripheral for one transmission, atomically.
 *
 * Use it before furi_hal_infrared_async_tx_set_data_isr_callback() and
 * furi_hal_infrared_async_tx_start(): between "is it busy?" and "start" nobody
 * else can take the peripheral (CLI, JS, another app), so there is no window in
 * which a second starter could reset the device. Clears every TX callback.
 *
 * The claim belongs to the calling TASK: setting the callbacks, starting and
 * releasing the claim must all happen in that task. Another task (even one that
 * does not claim) can neither start on it nor replace its callbacks. Stopping /
 * waiting for the transmission may be done from any task.
 *
 * Every successful claim ends in exactly one of
 *   - furi_hal_infrared_release_claim(), if the transmission is never started,
 *   - furi_hal_infrared_async_tx_stop() / _abort() / _wait_termination() after a
 *     start that returned true.
 *
 * @return false if the peripheral is busy (transmitting, receiving, claimed).
 */
bool furi_hal_infrared_claim_tx(void);

/** Give back a claim taken with furi_hal_infrared_claim_tx() that was never started. */
void furi_hal_infrared_release_claim(void);

/** Callback for a transmission that was cut short by a hardware error.
 *  Called once, from the TX task (not an ISR), before the transmission is
 *  reported finished. Valid for one transmission only; set it after the claim,
 *  before start. */
typedef void (*FuriHalInfraredTxErrorCallback)(void* context);
void furi_hal_infrared_async_tx_set_error_callback(
    FuriHalInfraredTxErrorCallback callback,
    void* context);

/** Start IR asynchronous transmission.
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
 * Return value:
 *  - false: the peripheral was busy (not claimed by the caller, in use by
 *    someone else). Nothing was started and nothing may be stopped or waited
 *    for: that would wait on the OTHER user's transmission.
 *  - true: the request was accepted and stop()/wait_termination() is due. It
 *    does not mean the signal is on the air: a request that could not be served
 *    (external pin owned by another function, board without IR, RMT setup
 *    failure) ends at once, and furi_hal_infrared_get_last_tx_error() tells why
 *    (FuriHalInfraredTxErrorNone if it is really transmitting).
 *
 * @param[in]  freq        frequency for PWM
 * @param[in]  duty_cycle  duty cycle for PWM
 */
bool furi_hal_infrared_async_tx_start(uint32_t freq, float duty_cycle);

/** Same as furi_hal_infrared_async_tx_start(), but without a carrier: a mark
 *  (level = true) drives the output pin high for exactly its duration, a space
 *  drives it low. Use it for plain pulse trains; the pin selection, the leases
 *  of shared pins and the error reporting are the same as for a carrier
 *  transmission. Return value: see furi_hal_infrared_async_tx_start(). */
bool furi_hal_infrared_async_tx_start_plain(void);

/** Stop IR asynchronous transmission and free resources.
 *
 * Transmission will stop as soon as transmission reaches end of package
 * (FuriHalInfraredTxGetDataStateDone or FuriHalInfraredTxGetDataStateLastDone).
 */
void furi_hal_infrared_async_tx_stop(void);

/** Stop IR asynchronous transmission at once and free resources.
 *
 * Unlike furi_hal_infrared_async_tx_stop() it does not wait for the end of the
 * package: the signal on the air is cut within about 20 ms. For callers whose
 * packages are long (continuous carrier / pulse trains), where a stop request
 * must not block for the length of a package. A message cut like this is
 * incomplete; never use it for protocol frames you want delivered.
 */
void furi_hal_infrared_async_tx_abort(void);

/** Forget the TX callbacks, but only while the peripheral is idle.
 *
 * The callbacks stay registered after a transmission. When they point into code
 * or data that is about to go away (an app that exits, an unloaded FAP), call
 * this after the final stop()/wait_termination(): a later user that does not
 * claim the peripheral would otherwise get a call into freed memory.
 * If the peripheral already belongs to someone else, their callbacks are left
 * alone.
 * @return true if the callbacks were cleared. */
bool furi_hal_infrared_async_tx_reset_callbacks(void);

/** True if the board has an infrared transmitter / receiver at all. On a board
 *  without IR a transmission request ends at once with
 *  FuriHalInfraredTxErrorNoHardware. */
bool furi_hal_infrared_is_available(void);

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

/** Detect which pin has an external IR module connected.
 *
 * @return FuriHalInfraredTxPinInternal (always, on ESP32).
 */
FuriHalInfraredTxPin furi_hal_infrared_detect_tx_output(void);

/** Set which pin will be used to transmit infrared signals.
 *
 * On boards with an external IR TX pin (BOARD_PIN_IR_TX_EXT: Paradoxt GPIO4,
 * T-Embed GPIO43) FuriHalInfraredTxPinExtPA7 selects that pin; elsewhere the
 * call is ignored. Takes effect at the next transmission, never mid-way.
 *
 * @param[in]   tx_pin  pin to be used for signal transmission.
 */
void furi_hal_infrared_set_tx_output(FuriHalInfraredTxPin tx_pin);

/** Currently selected TX output (FuriHalInfraredTxPinInternal on boards
 *  without an external TX pin). */
FuriHalInfraredTxPin furi_hal_infrared_get_tx_output(void);

/** True if the board can transmit on tx_pin. */
bool furi_hal_infrared_is_tx_output_supported(FuriHalInfraredTxPin tx_pin);

/** GPIO number of the external TX pin, or UINT16_MAX if there is none. */
uint16_t furi_hal_infrared_get_ext_tx_pin(void);

/** Store tx_pin as the boot-time default (NVS) and select it, so CLI, JS and
 *  plugins use the user's choice even before the Infrared app is opened.
 *  No-op on boards without an external TX pin. */
void furi_hal_infrared_save_tx_output(FuriHalInfraredTxPin tx_pin);

typedef enum {
    FuriHalInfraredTxErrorNone,
    FuriHalInfraredTxErrorExtPinBusy, /* external pin owned by another function */
    FuriHalInfraredTxErrorHardware, /* RMT channel could not be set up, or failed while sending */
    FuriHalInfraredTxErrorNoHardware, /* the board has no infrared transmitter */
} FuriHalInfraredTxError;

/** Why the last transmission did not start (FuriHalInfraredTxErrorNone if it
 *  did). A refused transmission ends at once: wait_termination() still works. */
FuriHalInfraredTxError furi_hal_infrared_get_last_tx_error(void);

#ifdef __cplusplus
}
#endif
