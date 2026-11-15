#ifndef FW_CFG_unit_tests

#include "nfc.h"

#include <furi_hal_nfc.h>
#include <furi/furi.h>

#define TAG "Nfc"

#define NFC_MAX_BUFFER_SIZE (256)

/* Safety cap on the block-Transmit one-shot wait below. The timer clears its
 * "running" flag from a GPTimer alarm ISR within microseconds and the wait has
 * to stay a tight spin, because stretching it would push the listener's frame
 * delay (FDT) past the reader's tolerance. The flag can only stay set forever
 * if that alarm is lost, so this is a wall-clock backstop, not a frame delay:
 * after it expires the transaction simply fails and the caller retries. */
#define NFC_BLOCK_TX_WAIT_TIMEOUT_MS (100)

#define NFC_FELICA_LISTENER_RESPONSE_TIME_A_FC (512 * 64)
#define NFC_FELICA_LISTENER_RESPONSE_TIME_B_FC (256 * 64)

typedef enum {
    NfcStateIdle,
    NfcStateRunning,
} NfcState;

typedef enum {
    NfcPollerStateStart,
    NfcPollerStateReady,
    NfcPollerStateReset,
    NfcPollerStateStop,

    NfcPollerStateNum,
} NfcPollerState;

typedef enum {
    NfcCommStateIdle,
    NfcCommStateWaitBlockTxTimer,
    NfcCommStateReadyTx,
    NfcCommStateWaitTxEnd,
    NfcCommStateWaitRxStart,
    NfcCommStateWaitRxEnd,
    NfcCommStateFailed,
} NfcCommState;

typedef enum {
    NfcConfigurationStateIdle,
    NfcConfigurationStateDone,
} NfcConfigurationState;

struct Nfc {
    NfcState state;
    NfcPollerState poller_state;
    NfcCommState comm_state;
    NfcConfigurationState config_state;
    NfcMode mode;

    uint32_t fdt_listen_fc;
    uint32_t mask_rx_time_fc;
    uint32_t fdt_poll_fc;
    uint32_t fdt_poll_poll_us;
    uint32_t guard_time_us;
    NfcEventCallback callback;
    void* context;

    uint8_t tx_buffer[NFC_MAX_BUFFER_SIZE];
    size_t tx_bits;
    uint8_t rx_buffer[NFC_MAX_BUFFER_SIZE];
    size_t rx_bits;

    FuriThread* worker_thread;
};

typedef bool (*NfcWorkerPollerStateHandler)(Nfc* instance);

static const FuriHalNfcTech nfc_tech_table[NfcModeNum][NfcTechNum] = {
    [NfcModePoller] =
        {
            [NfcTechIso14443a] = FuriHalNfcTechIso14443a,
            [NfcTechIso14443b] = FuriHalNfcTechIso14443b,
            [NfcTechIso15693] = FuriHalNfcTechIso15693,
            [NfcTechFelica] = FuriHalNfcTechFelica,
        },
    [NfcModeListener] =
        {
            [NfcTechIso14443a] = FuriHalNfcTechIso14443a,
            [NfcTechIso14443b] = FuriHalNfcTechInvalid,
            [NfcTechIso15693] = FuriHalNfcTechIso15693,
            [NfcTechFelica] = FuriHalNfcTechFelica,
        },
};

static NfcError nfc_process_hal_error(FuriHalNfcError error) {
    NfcError ret = NfcErrorNone;

    switch(error) {
    case FuriHalNfcErrorNone:
        ret = NfcErrorNone;
        break;
    case FuriHalNfcErrorIncompleteFrame:
        ret = NfcErrorIncompleteFrame;
        break;
    case FuriHalNfcErrorDataFormat:
        ret = NfcErrorDataFormat;
        break;
    case FuriHalNfcErrorCommunicationTimeout:
        ret = NfcErrorTimeout;
        break;

    default:
        ret = NfcErrorInternal;
    }

    return ret;
}

static int32_t nfc_worker_listener(void* context) {
    furi_assert(context);

    Nfc* instance = context;
    furi_assert(instance->callback);
    furi_assert(instance->config_state == NfcConfigurationStateDone);

    instance->state = NfcStateRunning;

    furi_hal_nfc_event_start();

    NfcEventData event_data = {};
    event_data.buffer = bit_buffer_alloc(NFC_MAX_BUFFER_SIZE);
    NfcEvent nfc_event = {.data = event_data};
    NfcCommand command = NfcCommandContinue;

    while(true) {
        FuriHalNfcEvent event = furi_hal_nfc_listener_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
        if(event & FuriHalNfcEventAbortRequest) {
            nfc_event.type = NfcEventTypeUserAbort;
            instance->callback(nfc_event, instance->context);
            break;
        }
        if(event & FuriHalNfcEventFieldOn) {
            nfc_event.type = NfcEventTypeFieldOn;
            instance->callback(nfc_event, instance->context);
        }
        if(event & FuriHalNfcEventFieldOff) {
            nfc_event.type = NfcEventTypeFieldOff;
            instance->callback(nfc_event, instance->context);
            furi_hal_nfc_listener_idle();
        }
        if(event & FuriHalNfcEventListenerActive) {
            nfc_event.type = NfcEventTypeListenerActivated;
            instance->callback(nfc_event, instance->context);
        }
        if(event & FuriHalNfcEventRxEnd) {
            furi_hal_nfc_timer_block_tx_start(instance->fdt_listen_fc);

            /* Only hand the frame to the protocol callback when the HAL actually
             * filled the buffer. On a FIFO overflow/empty read the HAL leaves
             * rx_bits untouched, so using it would feed stale (or, before the
             * first frame, uninitialised) length plus garbage bytes into
             * bit_buffer_copy_bits(), which asserts on a capacity mismatch. */
            if(furi_hal_nfc_listener_rx(
                   instance->rx_buffer, sizeof(instance->rx_buffer), &instance->rx_bits) ==
               FuriHalNfcErrorNone) {
                nfc_event.type = NfcEventTypeRxEnd;
                bit_buffer_copy_bits(event_data.buffer, instance->rx_buffer, instance->rx_bits);
                command = instance->callback(nfc_event, instance->context);
                if(command == NfcCommandStop) {
                    break;
                } else if(command == NfcCommandReset) {
                    furi_hal_nfc_listener_enable_rx();
                } else if(command == NfcCommandSleep) {
                    furi_hal_nfc_listener_idle();
                }
            }
        }
    }

    furi_hal_nfc_reset_mode();
    instance->config_state = NfcConfigurationStateIdle;

    bit_buffer_free(event_data.buffer);
    furi_hal_nfc_low_power_mode_start();
    return 0;
}

bool nfc_worker_poller_start_handler(Nfc* instance) {
    furi_hal_nfc_poller_field_on();
    if(instance->guard_time_us) {
        furi_hal_nfc_timer_block_tx_start_us(instance->guard_time_us);
        FuriHalNfcEvent event = furi_hal_nfc_poller_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
        if(event & FuriHalNfcEventAbortRequest) {
            /* nfc_stop() from another thread: abandon the guard-time wait (and
             * skip the expiry assert below) and unwind through the stop state. */
            instance->poller_state = NfcPollerStateStop;
            return false;
        }
        furi_assert(event & FuriHalNfcEventTimerBlockTxExpired);
    }
    instance->poller_state = NfcPollerStateReady;

    return false;
}

bool nfc_worker_poller_ready_handler(Nfc* instance) {
    NfcCommand command = NfcCommandContinue;

    NfcEvent event = {.type = NfcEventTypePollerReady};
    command = instance->callback(event, instance->context);
    if(command == NfcCommandReset) {
        instance->poller_state = NfcPollerStateReset;
    } else if(command == NfcCommandStop) {
        instance->poller_state = NfcPollerStateStop;
    }

    return false;
}

bool nfc_worker_poller_reset_handler(Nfc* instance) {
    furi_hal_nfc_low_power_mode_start();
    furi_delay_ms(100);
    furi_hal_nfc_low_power_mode_stop();
    instance->poller_state = NfcPollerStateStart;

    return false;
}

bool nfc_worker_poller_stop_handler(Nfc* instance) {
    FURI_LOG_I(TAG, "poller stop handler");
    furi_hal_nfc_reset_mode();
    instance->config_state = NfcConfigurationStateIdle;

    furi_hal_nfc_low_power_mode_start();
    // Wait after field is off some time to reset tag power
    furi_delay_ms(10);
    instance->poller_state = NfcPollerStateStart;

    return true;
}

static const NfcWorkerPollerStateHandler nfc_worker_poller_state_handlers[NfcPollerStateNum] = {
    [NfcPollerStateStart] = nfc_worker_poller_start_handler,
    [NfcPollerStateReady] = nfc_worker_poller_ready_handler,
    [NfcPollerStateReset] = nfc_worker_poller_reset_handler,
    [NfcPollerStateStop] = nfc_worker_poller_stop_handler,
};

static int32_t nfc_worker_poller(void* context) {
    furi_assert(context);

    Nfc* instance = context;
    furi_assert(instance->callback);
    instance->state = NfcStateRunning;
    instance->poller_state = NfcPollerStateStart;

    furi_hal_nfc_event_start();

    bool exit = false;
    while(!exit) {
        exit = nfc_worker_poller_state_handlers[instance->poller_state](instance);
        /* Pace the state loop: when the NFC chip stops answering, handlers
         * return instantly and this loop - running at the highest thread
         * priority - would spin flat-out and starve the entire system.
         * 1 ms per state round is invisible to card polling. */
        furi_delay_ms(1);
    }

    FURI_LOG_I(TAG, "poller worker exit");
    return 0;
}

Nfc* nfc_alloc(void) {
    furi_check(furi_hal_nfc_acquire() == FuriHalNfcErrorNone);

    Nfc* instance = malloc(sizeof(Nfc));
    instance->state = NfcStateIdle;
    instance->comm_state = NfcCommStateIdle;
    instance->config_state = NfcConfigurationStateIdle;
    /* Do not rely on the allocator zero-filling: a stale rx_bits is fed to
     * bit_buffer_copy_bits() on the first frame if the HAL read fails. */
    instance->tx_bits = 0;
    instance->rx_bits = 0;

    instance->worker_thread = furi_thread_alloc();
    furi_thread_set_name(instance->worker_thread, "NfcWorker");
    furi_thread_set_context(instance->worker_thread, instance);
    furi_thread_set_priority(instance->worker_thread, FuriThreadPriorityHighest);
    furi_thread_set_stack_size(instance->worker_thread, 8 * 1024);

    return instance;
}

void nfc_free(Nfc* instance) {
    furi_check(instance);
    furi_check(instance->state == NfcStateIdle);

    furi_thread_free(instance->worker_thread);
    free(instance);

    furi_hal_nfc_release();
}

void nfc_config(Nfc* instance, NfcMode mode, NfcTech tech) {
    furi_check(instance);
    furi_check(mode < NfcModeNum);
    furi_check(tech < NfcTechNum);
    furi_check(instance->config_state == NfcConfigurationStateIdle);

    FuriHalNfcTech hal_tech = nfc_tech_table[mode][tech];
    if(hal_tech == FuriHalNfcTechInvalid) {
        furi_crash("Unsupported mode for given tech");
    }
    FuriHalNfcMode hal_mode = (mode == NfcModePoller) ? FuriHalNfcModePoller :
                                                        FuriHalNfcModeListener;
    furi_hal_nfc_low_power_mode_stop();
    furi_hal_nfc_set_mode(hal_mode, hal_tech);

    instance->mode = mode;
    instance->config_state = NfcConfigurationStateDone;
}

void nfc_set_fdt_poll_fc(Nfc* instance, uint32_t fdt_poll_fc) {
    furi_check(instance);
    instance->fdt_poll_fc = fdt_poll_fc;
}

void nfc_set_fdt_listen_fc(Nfc* instance, uint32_t fdt_listen_fc) {
    furi_check(instance);
    instance->fdt_listen_fc = fdt_listen_fc;
}

void nfc_set_fdt_poll_poll_us(Nfc* instance, uint32_t fdt_poll_poll_us) {
    furi_check(instance);
    instance->fdt_poll_poll_us = fdt_poll_poll_us;
}

void nfc_set_guard_time_us(Nfc* instance, uint32_t guard_time_us) {
    furi_check(instance);
    instance->guard_time_us = guard_time_us;
}

void nfc_set_mask_receive_time_fc(Nfc* instance, uint32_t mask_rx_time_fc) {
    furi_check(instance);
    instance->mask_rx_time_fc = mask_rx_time_fc;
}

void nfc_start(Nfc* instance, NfcEventCallback callback, void* context) {
    furi_check(instance);
    furi_check(instance->worker_thread);
    furi_check(callback);
    furi_check(instance->config_state == NfcConfigurationStateDone);

    instance->callback = callback;
    instance->context = context;
    if(instance->mode == NfcModePoller) {
        furi_thread_set_callback(instance->worker_thread, nfc_worker_poller);
    } else {
        furi_thread_set_callback(instance->worker_thread, nfc_worker_listener);
    }
    instance->comm_state = NfcCommStateIdle;
    furi_thread_start(instance->worker_thread);
}

static void nfc_stop_impl(Nfc* instance, bool request_abort) {
    furi_check(instance);
    furi_check(instance->state == NfcStateRunning);

    if(request_abort) {
        /* Wake the worker out of any FURI_HAL_NFC_EVENT_WAIT_FOREVER wait in both
         * modes. furi_hal_nfc_poller_wait_event() honours FuriHalNfcEventAbortRequest
         * like any other event bit, but the poller path never requested it, so a
         * worker parked on a stalled chip/card made the furi_thread_join() below
         * hang the calling (GUI) thread forever - a total freeze on exiting any
         * NFC app. */
        furi_hal_nfc_abort();
    }

    /* On ESP32 with FreeRTOS preemptive scheduling, view_dispatcher_send_custom_event
     * from a worker callback can trigger synchronous event processing in the main thread
     * (higher priority). If the event handler calls nfc_stop, we'd be trying to join
     * the worker thread from within itself → deadlock.
     * Skip the join if called from the worker thread; it will exit naturally. */
    FuriThreadId current = furi_thread_get_current_id();
    FuriThreadId worker = furi_thread_get_id(instance->worker_thread);
    FURI_LOG_D("Nfc", "nfc_stop: current=%p worker=%p", (void*)current, (void*)worker);
    if(current != worker) {
        furi_thread_join(instance->worker_thread);
    } else {
        FURI_LOG_W("Nfc", "nfc_stop: skipping join (same thread)");
    }

    instance->state = NfcStateIdle;
}

void nfc_stop(Nfc* instance) {
    nfc_stop_impl(instance, true);
}

void nfc_stop_no_abort(Nfc* instance) {
    nfc_stop_impl(instance, false);
}

NfcError nfc_listener_tx(Nfc* instance, const BitBuffer* tx_buffer) {
    furi_check(instance);
    furi_check(tx_buffer);

    NfcError ret = NfcErrorNone;

    uint32_t block_tx_deadline = furi_get_tick() + NFC_BLOCK_TX_WAIT_TIMEOUT_MS;
    while(furi_hal_nfc_timer_block_tx_is_running()) {
        if((int32_t)(furi_get_tick() - block_tx_deadline) >= 0) break;
    }

    FuriHalNfcError error =
        furi_hal_nfc_listener_tx(bit_buffer_get_data(tx_buffer), bit_buffer_get_size(tx_buffer));
    if(error != FuriHalNfcErrorNone) {
        FURI_LOG_D(TAG, "Failed in listener TX");
        ret = nfc_process_hal_error(error);
    }

    return ret;
}

static NfcError nfc_poller_trx_state_machine(Nfc* instance, uint32_t fwt_fc) {
    FuriHalNfcEvent event = 0;
    NfcError error = NfcErrorNone;

    while(true) {
        event = furi_hal_nfc_poller_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
        if(event & FuriHalNfcEventAbortRequest) {
            /* nfc_stop() while a transaction is in flight: abandon it so the
             * poller callback chain unwinds and sees the stop request. */
            error = NfcErrorTimeout;
            break;
        }
        if(event & FuriHalNfcEventTimerBlockTxExpired) {
            if(instance->comm_state == NfcCommStateWaitBlockTxTimer) {
                instance->comm_state = NfcCommStateReadyTx;
            }
        }
        if(event & FuriHalNfcEventTxEnd) {
            if(instance->comm_state == NfcCommStateWaitTxEnd) {
                if(fwt_fc) {
                    furi_hal_nfc_timer_fwt_start(fwt_fc);
                }
                furi_hal_nfc_timer_block_tx_start_us(instance->fdt_poll_poll_us);
                instance->comm_state = NfcCommStateWaitRxStart;
            }
        }
        if(event & FuriHalNfcEventRxStart) {
            if(instance->comm_state == NfcCommStateWaitRxStart) {
                furi_hal_nfc_timer_block_tx_stop();
                furi_hal_nfc_timer_fwt_stop();
                instance->comm_state = NfcCommStateWaitRxEnd;
            }
        }
        if(event & FuriHalNfcEventRxEnd) {
            furi_hal_nfc_timer_block_tx_start(instance->fdt_poll_fc);
            furi_hal_nfc_timer_fwt_stop();
            instance->comm_state = NfcCommStateWaitBlockTxTimer;
            break;
        }
        if(event & FuriHalNfcEventTimerFwtExpired) {
            if(instance->comm_state == NfcCommStateWaitRxStart) {
                error = NfcErrorTimeout;
                FURI_LOG_D(TAG, "FWT Timeout");
                if(furi_hal_nfc_timer_block_tx_is_running()) {
                    instance->comm_state = NfcCommStateWaitBlockTxTimer;
                } else {
                    instance->comm_state = NfcCommStateReadyTx;
                }
                break;
            }
        }
    }

    return error;
}

NfcError nfc_iso14443a_poller_trx_custom_parity(
    Nfc* instance,
    const BitBuffer* tx_buffer,
    BitBuffer* rx_buffer,
    uint32_t fwt) {
    furi_check(instance);
    furi_check(tx_buffer);
    furi_check(rx_buffer);

    furi_check(instance->poller_state == NfcPollerStateReady);

    NfcError ret = NfcErrorNone;
    FuriHalNfcError error = FuriHalNfcErrorNone;
    do {
        furi_hal_nfc_trx_reset();
        while(furi_hal_nfc_timer_block_tx_is_running()) {
            FuriHalNfcEvent event =
                furi_hal_nfc_poller_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
            if(event & (FuriHalNfcEventTimerBlockTxExpired | FuriHalNfcEventAbortRequest)) {
                break;
            }
        }
        bit_buffer_write_bytes_with_parity(
            tx_buffer, instance->tx_buffer, sizeof(instance->tx_buffer), &instance->tx_bits);
        error =
            furi_hal_nfc_iso14443a_poller_tx_custom_parity(instance->tx_buffer, instance->tx_bits);
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_D(TAG, "Failed in poller TX");
            ret = nfc_process_hal_error(error);
            break;
        }
        instance->comm_state = NfcCommStateWaitTxEnd;
        ret = nfc_poller_trx_state_machine(instance, fwt);
        if(ret != NfcErrorNone) {
            FURI_LOG_T(TAG, "Failed TRX state machine");
            break;
        }

        error = furi_hal_nfc_poller_rx(
            instance->rx_buffer, sizeof(instance->rx_buffer), &instance->rx_bits);
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_D(TAG, "Failed in poller RX");
            ret = nfc_process_hal_error(error);
            break;
        }
        if(instance->rx_bits >= 9) {
            if((instance->rx_bits % 9) != 0) {
                ret = NfcErrorDataFormat;
                break;
            }
        }

        bit_buffer_copy_bytes_with_parity(rx_buffer, instance->rx_buffer, instance->rx_bits);
    } while(false);

    return ret;
}

NfcError
    nfc_poller_trx(Nfc* instance, const BitBuffer* tx_buffer, BitBuffer* rx_buffer, uint32_t fwt) {
    furi_check(instance);
    furi_check(tx_buffer);
    furi_check(rx_buffer);

    furi_check(instance->poller_state == NfcPollerStateReady);

    NfcError ret = NfcErrorNone;
    FuriHalNfcError error = FuriHalNfcErrorNone;
    do {
        furi_hal_nfc_trx_reset();
        while(furi_hal_nfc_timer_block_tx_is_running()) {
            FuriHalNfcEvent event =
                furi_hal_nfc_poller_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
            if(event & (FuriHalNfcEventTimerBlockTxExpired | FuriHalNfcEventAbortRequest)) {
                break;
            }
        }
        error =
            furi_hal_nfc_poller_tx(bit_buffer_get_data(tx_buffer), bit_buffer_get_size(tx_buffer));
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_D(TAG, "Failed in poller TX");
            ret = nfc_process_hal_error(error);
            break;
        }
        instance->comm_state = NfcCommStateWaitTxEnd;
        ret = nfc_poller_trx_state_machine(instance, fwt);
        if(ret != NfcErrorNone) {
            FURI_LOG_T(TAG, "Failed TRX state machine");
            break;
        }

        error = furi_hal_nfc_poller_rx(
            instance->rx_buffer, sizeof(instance->rx_buffer), &instance->rx_bits);
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_D(TAG, "Failed in poller RX");
            ret = nfc_process_hal_error(error);
            break;
        }

        bit_buffer_copy_bits(rx_buffer, instance->rx_buffer, instance->rx_bits);
    } while(false);

    return ret;
}

NfcError nfc_iso14443a_listener_set_col_res_data(
    Nfc* instance,
    uint8_t* uid,
    uint8_t uid_len,
    uint8_t* atqa,
    uint8_t sak) {
    furi_check(instance);

    FuriHalNfcError error =
        furi_hal_nfc_iso14443a_listener_set_col_res_data(uid, uid_len, atqa, sak);
    instance->comm_state = NfcCommStateIdle;
    return nfc_process_hal_error(error);
}

NfcError nfc_iso14443a_poller_trx_short_frame(
    Nfc* instance,
    NfcIso14443aShortFrame frame,
    BitBuffer* rx_buffer,
    uint32_t fwt) {
    furi_check(instance);
    furi_check(rx_buffer);

    FuriHalNfcaShortFrame short_frame = (frame == NfcIso14443aShortFrameAllReqa) ?
                                            FuriHalNfcaShortFrameAllReq :
                                            FuriHalNfcaShortFrameSensReq;

    furi_check(instance->poller_state == NfcPollerStateReady);

    NfcError ret = NfcErrorNone;
    FuriHalNfcError error = FuriHalNfcErrorNone;
    do {
        furi_hal_nfc_trx_reset();
        while(furi_hal_nfc_timer_block_tx_is_running()) {
            FuriHalNfcEvent event =
                furi_hal_nfc_poller_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
            if(event & (FuriHalNfcEventTimerBlockTxExpired | FuriHalNfcEventAbortRequest)) {
                break;
            }
        }
        error = furi_hal_nfc_iso14443a_poller_trx_short_frame(short_frame);
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_D(TAG, "Failed in poller TX");
            ret = nfc_process_hal_error(error);
            break;
        }
        instance->comm_state = NfcCommStateWaitTxEnd;
        ret = nfc_poller_trx_state_machine(instance, fwt);
        if(ret != NfcErrorNone) {
            FURI_LOG_T(TAG, "Failed TRX state machine");
            break;
        }

        error = furi_hal_nfc_poller_rx(
            instance->rx_buffer, sizeof(instance->rx_buffer), &instance->rx_bits);
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_D(TAG, "Failed in poller RX");
            ret = nfc_process_hal_error(error);
            break;
        }

        bit_buffer_copy_bits(rx_buffer, instance->rx_buffer, instance->rx_bits);
    } while(false);

    return ret;
}

NfcError nfc_iso14443a_poller_trx_sdd_frame(
    Nfc* instance,
    const BitBuffer* tx_buffer,
    BitBuffer* rx_buffer,
    uint32_t fwt) {
    furi_check(instance);
    furi_check(tx_buffer);
    furi_check(rx_buffer);

    furi_check(instance->poller_state == NfcPollerStateReady);

    NfcError ret = NfcErrorNone;
    FuriHalNfcError error = FuriHalNfcErrorNone;
    do {
        furi_hal_nfc_trx_reset();
        while(furi_hal_nfc_timer_block_tx_is_running()) {
            FuriHalNfcEvent event =
                furi_hal_nfc_poller_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
            if(event & (FuriHalNfcEventTimerBlockTxExpired | FuriHalNfcEventAbortRequest)) {
                break;
            }
        }
        error = furi_hal_nfc_iso14443a_tx_sdd_frame(
            bit_buffer_get_data(tx_buffer), bit_buffer_get_size(tx_buffer));
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_D(TAG, "Failed in poller TX");
            ret = nfc_process_hal_error(error);
            break;
        }
        instance->comm_state = NfcCommStateWaitTxEnd;
        ret = nfc_poller_trx_state_machine(instance, fwt);
        if(ret != NfcErrorNone) {
            FURI_LOG_T(TAG, "Failed TRX state machine");
            break;
        }

        error = furi_hal_nfc_poller_rx(
            instance->rx_buffer, sizeof(instance->rx_buffer), &instance->rx_bits);
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_D(TAG, "Failed in poller RX");
            ret = nfc_process_hal_error(error);
            break;
        }

        bit_buffer_copy_bits(rx_buffer, instance->rx_buffer, instance->rx_bits);
    } while(false);

    return ret;
}

NfcError nfc_iso14443a_listener_tx_custom_parity(Nfc* instance, const BitBuffer* tx_buffer) {
    furi_check(instance);
    furi_check(tx_buffer);

    NfcError ret = NfcErrorNone;
    FuriHalNfcError error = FuriHalNfcErrorNone;

    const uint8_t* tx_data = bit_buffer_get_data(tx_buffer);
    const uint8_t* tx_parity = bit_buffer_get_parity(tx_buffer);
    size_t tx_bits = bit_buffer_get_size(tx_buffer);

    error = furi_hal_nfc_iso14443a_listener_tx_custom_parity(tx_data, tx_parity, tx_bits);
    ret = nfc_process_hal_error(error);

    return ret;
}

NfcError nfc_iso15693_listener_tx_sof(Nfc* instance) {
    furi_check(instance);

    uint32_t block_tx_deadline = furi_get_tick() + NFC_BLOCK_TX_WAIT_TIMEOUT_MS;
    while(furi_hal_nfc_timer_block_tx_is_running()) {
        if((int32_t)(furi_get_tick() - block_tx_deadline) >= 0) break;
    }

    FuriHalNfcError error = furi_hal_nfc_iso15693_listener_tx_sof();
    NfcError ret = nfc_process_hal_error(error);

    return ret;
}

NfcError nfc_felica_listener_set_sensf_res_data(
    Nfc* instance,
    const uint8_t* idm,
    const uint8_t idm_len,
    const uint8_t* pmm,
    const uint8_t pmm_len,
    const uint16_t sys_code) {
    furi_check(instance);

    FuriHalNfcError error =
        furi_hal_nfc_felica_listener_set_sensf_res_data(idm, idm_len, pmm, pmm_len, sys_code);
    instance->comm_state = NfcCommStateIdle;
    return nfc_process_hal_error(error);
}

void nfc_felica_listener_timer_anticol_start(Nfc* instance, uint8_t target_time_slot) {
    furi_check(instance);

    furi_hal_nfc_timer_block_tx_start(
        NFC_FELICA_LISTENER_RESPONSE_TIME_A_FC +
        target_time_slot * NFC_FELICA_LISTENER_RESPONSE_TIME_B_FC);
}

void nfc_felica_listener_timer_anticol_stop(Nfc* instance) {
    furi_check(instance);

    if(furi_hal_nfc_timer_block_tx_is_running()) {
        furi_hal_nfc_timer_block_tx_stop();
    }
}

#endif // FW_CFG_unit_tests
