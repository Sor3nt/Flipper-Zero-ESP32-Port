#ifdef ESP_PLATFORM
#include <boards/board.h>
#endif

#if defined(ESP_PLATFORM) && defined(BOARD_HAS_EXPANSION_UART) && BOARD_HAS_EXPANSION_UART

#include "expansion_worker.h"

#include <driver/uart.h>
#include <esp_timer.h>
#include <furi_hal_power.h>
#include <furi_hal_uart_shared.h>
#include <furi.h>
#include <power/power_service/power.h>
#include <rpc/rpc.h>

#include "expansion_protocol.h"

#define TAG "ExpansionSrv"
#define EXPANSION_WORKER_STACK_SIZE (4096UL)
#define EXPANSION_WORKER_UART       BOARD_EXPANSION_UART
#define EXPANSION_WORKER_RX_BUFFER  512
#define EXPANSION_WORKER_TX_BUFFER  512
#define EXPANSION_WORKER_READ_SLICE_MS 25U

typedef enum {
    ExpansionWorkerStateHandShake,
    ExpansionWorkerStateConnected,
    ExpansionWorkerStateRpcActive,
} ExpansionWorkerState;

typedef enum {
    ExpansionWorkerExitReasonUnknown,
    ExpansionWorkerExitReasonUser,
    ExpansionWorkerExitReasonPreempted,
    ExpansionWorkerExitReasonError,
    ExpansionWorkerExitReasonTimeout,
} ExpansionWorkerExitReason;

struct ExpansionWorker {
    FuriThread* thread;
    RpcSession* rpc_session;
    FuriSemaphore* tx_semaphore;
    FuriMutex* tx_mutex;
    FuriHalSerialId serial_id;
    ExpansionWorkerState state;
    ExpansionWorkerExitReason exit_reason;
    ExpansionWorkerCallback callback;
    void* cb_context;
    volatile bool stopping;
    bool uart_claimed;
    bool uart_installed;
    bool rpc_record_open;
    uint32_t baud_rate;
};

static bool expansion_worker_should_stop(ExpansionWorker* instance) {
    if(instance->stopping) {
        instance->exit_reason = ExpansionWorkerExitReasonUser;
        return true;
    }
    if(furi_hal_uart_shared_preemption_requested()) {
        instance->exit_reason = ExpansionWorkerExitReasonPreempted;
        return true;
    }
    return false;
}

static size_t expansion_worker_receive_callback(uint8_t* data, size_t data_size, void* context) {
    ExpansionWorker* instance = context;
    const int64_t deadline = esp_timer_get_time() +
                             (int64_t)EXPANSION_PROTOCOL_TIMEOUT_MS * 1000;
    size_t received = 0;

    while(received < data_size) {
        if(expansion_worker_should_stop(instance)) break;
        const int64_t remaining_us = deadline - esp_timer_get_time();
        if(remaining_us <= 0) {
            instance->exit_reason = ExpansionWorkerExitReasonTimeout;
            break;
        }

        uint32_t wait_ms = (uint32_t)((remaining_us + 999) / 1000);
        if(wait_ms > EXPANSION_WORKER_READ_SLICE_MS) {
            wait_ms = EXPANSION_WORKER_READ_SLICE_MS;
        }
        const int count = uart_read_bytes(
            EXPANSION_WORKER_UART,
            data + received,
            data_size - received,
            pdMS_TO_TICKS(wait_ms));
        if(count < 0) {
            FURI_LOG_E(TAG, "UART read failed during expansion frame");
            instance->exit_reason = ExpansionWorkerExitReasonError;
            break;
        }
        received += (size_t)count;
    }
    return received;
}

static size_t expansion_worker_send_callback(
    const uint8_t* data,
    size_t data_size,
    void* context) {
    ExpansionWorker* instance = context;
    const int count = uart_write_bytes(EXPANSION_WORKER_UART, data, data_size);
    if(count < 0 || (size_t)count != data_size) {
        FURI_LOG_E(TAG, "UART write failed during expansion frame");
        instance->exit_reason = ExpansionWorkerExitReasonError;
        return 0;
    }
    if(uart_wait_tx_done(
           EXPANSION_WORKER_UART, pdMS_TO_TICKS(EXPANSION_PROTOCOL_TIMEOUT_MS)) != ESP_OK) {
        FURI_LOG_E(TAG, "UART TX timed out during expansion frame");
        instance->exit_reason = ExpansionWorkerExitReasonError;
        return 0;
    }
    return data_size;
}

static bool expansion_worker_send_frame(ExpansionWorker* instance, const ExpansionFrame* frame) {
    furi_check(furi_mutex_acquire(instance->tx_mutex, FuriWaitForever) == FuriStatusOk);
    const bool sent =
        expansion_protocol_encode(frame, expansion_worker_send_callback, instance) ==
        ExpansionProtocolStatusOk;
    furi_check(furi_mutex_release(instance->tx_mutex) == FuriStatusOk);
    return sent;
}

static bool expansion_worker_receive_frame(ExpansionWorker* instance, ExpansionFrame* frame) {
    return expansion_protocol_decode(frame, expansion_worker_receive_callback, instance) ==
           ExpansionProtocolStatusOk;
}

static bool expansion_worker_send_heartbeat(ExpansionWorker* instance) {
    const ExpansionFrame frame = {
        .header.type = ExpansionFrameTypeHeartbeat,
        .content.heartbeat = {},
    };
    return expansion_worker_send_frame(instance, &frame);
}

static bool expansion_worker_send_status_response(
    ExpansionWorker* instance,
    ExpansionFrameError error) {
    const ExpansionFrame frame = {
        .header.type = ExpansionFrameTypeStatus,
        .content.status.error = error,
    };
    return expansion_worker_send_frame(instance, &frame);
}

static bool expansion_worker_send_data_response(
    ExpansionWorker* instance,
    const uint8_t* data,
    size_t data_size) {
    furi_assert(data_size <= EXPANSION_PROTOCOL_MAX_DATA_SIZE);
    ExpansionFrame frame = {
        .header.type = ExpansionFrameTypeData,
        .content.data.size = data_size,
    };
    memcpy(frame.content.data.bytes, data, data_size);
    return expansion_worker_send_frame(instance, &frame);
}

static void expansion_worker_rpc_send_callback(void* context, uint8_t* data, size_t data_size) {
    ExpansionWorker* instance = context;

    for(size_t sent = 0; sent < data_size;) {
        if(furi_semaphore_acquire(
               instance->tx_semaphore, furi_ms_to_ticks(EXPANSION_PROTOCOL_TIMEOUT_MS)) !=
           FuriStatusOk) {
            instance->exit_reason = ExpansionWorkerExitReasonError;
            break;
        }
        const size_t chunk =
            MIN(data_size - sent, (size_t)EXPANSION_PROTOCOL_MAX_DATA_SIZE);
        if(!expansion_worker_send_data_response(instance, data + sent, chunk)) {
            furi_check(furi_semaphore_release(instance->tx_semaphore) == FuriStatusOk);
            break;
        }
        sent += chunk;
    }
}

static bool expansion_worker_rpc_session_open(ExpansionWorker* instance) {
    Rpc* rpc = furi_record_open(RECORD_RPC);
    instance->rpc_record_open = true;
    instance->rpc_session = rpc_session_open(rpc, RpcOwnerUart);
    if(!instance->rpc_session) return false;

    instance->tx_semaphore = furi_semaphore_alloc(1, 1);
    if(!instance->tx_semaphore) {
        rpc_session_close(instance->rpc_session);
        instance->rpc_session = NULL;
        return false;
    }
    rpc_session_set_context(instance->rpc_session, instance);
    rpc_session_set_send_bytes_callback(
        instance->rpc_session, expansion_worker_rpc_send_callback);
    return true;
}

static void expansion_worker_rpc_session_close(ExpansionWorker* instance) {
    if(instance->rpc_session) {
        rpc_session_close(instance->rpc_session);
        instance->rpc_session = NULL;
    }
    if(instance->tx_semaphore) {
        furi_semaphore_free(instance->tx_semaphore);
        instance->tx_semaphore = NULL;
    }
    if(instance->rpc_record_open) {
        furi_record_close(RECORD_RPC);
        instance->rpc_record_open = false;
    }
}

static bool expansion_worker_handle_state_handshake(
    ExpansionWorker* instance,
    const ExpansionFrame* frame) {
    if(frame->header.type != ExpansionFrameTypeBaudRate) return false;
    const uint32_t requested_baud = frame->content.baud_rate.baud;

    if(requested_baud < 1200 || requested_baud > 2000000) {
        return expansion_worker_send_status_response(instance, ExpansionFrameErrorBaudRate);
    }

    if(!expansion_worker_send_status_response(instance, ExpansionFrameErrorNone)) return false;
    if(uart_set_baudrate(EXPANSION_WORKER_UART, requested_baud) != ESP_OK) {
        FURI_LOG_E(TAG, "Unsupported expansion baud rate: %lu", (unsigned long)requested_baud);
        return false;
    }

    instance->baud_rate = requested_baud;
    instance->state = ExpansionWorkerStateConnected;
    if(instance->callback) {
        instance->callback(instance->cb_context, ExpansionWorkerCallbackReasonConnected);
    }
    return true;
}

static bool expansion_worker_handle_state_connected(
    ExpansionWorker* instance,
    const ExpansionFrame* frame) {
    if(frame->header.type == ExpansionFrameTypeControl) {
        const uint8_t command = frame->content.control.command;
        if(command == ExpansionFrameControlCommandStartRpc) {
            if(!expansion_worker_rpc_session_open(instance)) return false;
            instance->state = ExpansionWorkerStateRpcActive;
        } else if(command == ExpansionFrameControlCommandEnableOtg ||
                  command == ExpansionFrameControlCommandDisableOtg) {
            Power* power = furi_record_open(RECORD_POWER);
            power_enable_otg(power, command == ExpansionFrameControlCommandEnableOtg);
            furi_record_close(RECORD_POWER);
        } else {
            return false;
        }
        return expansion_worker_send_status_response(instance, ExpansionFrameErrorNone);
    }
    if(frame->header.type == ExpansionFrameTypeHeartbeat) {
        return expansion_worker_send_heartbeat(instance);
    }
    return false;
}

static bool expansion_worker_handle_state_rpc_active(
    ExpansionWorker* instance,
    const ExpansionFrame* frame) {
    if(frame->header.type == ExpansionFrameTypeData) {
        if(!expansion_worker_send_status_response(instance, ExpansionFrameErrorNone)) return false;
        const size_t consumed = rpc_session_feed(
            instance->rpc_session,
            frame->content.data.bytes,
            frame->content.data.size,
            EXPANSION_PROTOCOL_TIMEOUT_MS);
        return consumed == frame->content.data.size;
    }
    if(frame->header.type == ExpansionFrameTypeControl &&
       frame->content.control.command == ExpansionFrameControlCommandStopRpc) {
        expansion_worker_rpc_session_close(instance);
        instance->state = ExpansionWorkerStateConnected;
        return expansion_worker_send_status_response(instance, ExpansionFrameErrorNone);
    }
    if(frame->header.type == ExpansionFrameTypeStatus &&
       frame->content.status.error == ExpansionFrameErrorNone) {
        return furi_semaphore_release(instance->tx_semaphore) == FuriStatusOk;
    }
    if(frame->header.type == ExpansionFrameTypeHeartbeat) {
        return expansion_worker_send_heartbeat(instance);
    }
    return false;
}

typedef bool (*ExpansionWorkerStateHandler)(ExpansionWorker*, const ExpansionFrame*);

static const ExpansionWorkerStateHandler expansion_worker_handlers[] = {
    [ExpansionWorkerStateHandShake] = expansion_worker_handle_state_handshake,
    [ExpansionWorkerStateConnected] = expansion_worker_handle_state_connected,
    [ExpansionWorkerStateRpcActive] = expansion_worker_handle_state_rpc_active,
};

static void expansion_worker_state_machine(ExpansionWorker* instance) {
    ExpansionFrame frame;
    while(!expansion_worker_should_stop(instance)) {
        if(!expansion_worker_receive_frame(instance, &frame)) break;
        if(!expansion_worker_handlers[instance->state](instance, &frame)) {
            instance->exit_reason = ExpansionWorkerExitReasonError;
            break;
        }
    }
}

static int32_t expansion_worker_thread(void* context) {
    ExpansionWorker* instance = context;
    furi_hal_power_insomnia_enter();
    instance->uart_claimed =
        furi_hal_uart_shared_acquire_background(0) == FuriHalUartSharedAcquireAcquired;
    if(!instance->uart_claimed) {
        FURI_LOG_W(TAG, "Shared UART is busy; cannot start expansion worker");
        instance->exit_reason = ExpansionWorkerExitReasonError;
        goto finish;
    }

    esp_err_t err = uart_driver_install(
        EXPANSION_WORKER_UART,
        EXPANSION_WORKER_RX_BUFFER,
        EXPANSION_WORKER_TX_BUFFER,
        0,
        NULL,
        0);
    if(err == ESP_OK) {
        instance->uart_installed = true;
        err = uart_param_config(EXPANSION_WORKER_UART, &(uart_config_t){
            .baud_rate = EXPANSION_PROTOCOL_DEFAULT_BAUD_RATE,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        });
    }
    if(err == ESP_OK) {
        err = uart_set_pin(
            EXPANSION_WORKER_UART,
            BOARD_PIN_EXPANSION_TX,
            BOARD_PIN_EXPANSION_RX,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE);
    }
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "Expansion UART initialization failed: %s", esp_err_to_name(err));
        instance->exit_reason = ExpansionWorkerExitReasonError;
        goto finish;
    }

    uart_flush_input(EXPANSION_WORKER_UART);
    instance->baud_rate = EXPANSION_PROTOCOL_DEFAULT_BAUD_RATE;
    instance->state = ExpansionWorkerStateHandShake;
    instance->exit_reason = ExpansionWorkerExitReasonUnknown;
    if(expansion_worker_send_heartbeat(instance)) {
        expansion_worker_state_machine(instance);
    } else {
        instance->exit_reason = ExpansionWorkerExitReasonError;
    }

finish:
    if(instance->rpc_session || instance->rpc_record_open) {
        expansion_worker_rpc_session_close(instance);
    }
    if(instance->uart_installed) {
        uart_wait_tx_done(EXPANSION_WORKER_UART, pdMS_TO_TICKS(100));
        err = uart_driver_delete(EXPANSION_WORKER_UART);
        if(err != ESP_OK) {
            FURI_LOG_E(TAG, "Could not remove Expansion UART driver: %s", esp_err_to_name(err));
            instance->uart_installed = false;
            instance->uart_claimed = false;
        } else {
            instance->uart_installed = false;
        }
    }
    if(instance->uart_claimed && !furi_hal_uart_shared_release()) {
        FURI_LOG_E(TAG, "Could not release shared UART after Expansion");
    }
    furi_hal_power_insomnia_exit();
    if(instance->exit_reason != ExpansionWorkerExitReasonUser && instance->callback) {
        instance->callback(instance->cb_context, ExpansionWorkerCallbackReasonExit);
    }
    return 0;
}

ExpansionWorker* expansion_worker_alloc(FuriHalSerialId serial_id) {
    furi_check(serial_id == FuriHalSerialIdUsart);
    ExpansionWorker* instance = calloc(1, sizeof(ExpansionWorker));
    if(!instance) return NULL;
    instance->serial_id = serial_id;
    instance->tx_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!instance->tx_mutex) {
        free(instance);
        return NULL;
    }
    instance->thread = furi_thread_alloc_ex(
        TAG "Worker", EXPANSION_WORKER_STACK_SIZE, expansion_worker_thread, instance);
    if(!instance->thread) {
        furi_mutex_free(instance->tx_mutex);
        free(instance);
        return NULL;
    }
    instance->exit_reason = ExpansionWorkerExitReasonUnknown;
    return instance;
}

void expansion_worker_free(ExpansionWorker* instance) {
    furi_assert(instance);
    furi_thread_join(instance->thread);
    furi_thread_free(instance->thread);
    furi_mutex_free(instance->tx_mutex);
    free(instance);
}

void expansion_worker_set_callback(
    ExpansionWorker* instance,
    ExpansionWorkerCallback callback,
    void* context) {
    furi_assert(instance);
    instance->callback = callback;
    instance->cb_context = context;
}

void expansion_worker_start(ExpansionWorker* instance) {
    furi_assert(instance);
    furi_thread_start(instance->thread);
}

void expansion_worker_stop(ExpansionWorker* instance) {
    furi_assert(instance);
    instance->stopping = true;
    if(furi_thread_get_state(instance->thread) == FuriThreadStateRunning) {
        furi_thread_join(instance->thread);
    }
    instance->exit_reason = ExpansionWorkerExitReasonUser;
}

#elif !defined(ESP_PLATFORM)

#include "expansion_worker.h"

#include <power/power_service/power.h>
#include <furi_hal_power.h>

#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>

#include <furi.h>
#include <rpc/rpc.h>

#include "expansion_protocol.h"

#define TAG "ExpansionSrv"

#define EXPANSION_WORKER_STACK_SZIE  (768UL)
#define EXPANSION_WORKER_BUFFER_SIZE (sizeof(ExpansionFrame) + sizeof(ExpansionFrameChecksum))

typedef enum {
    ExpansionWorkerStateHandShake,
    ExpansionWorkerStateConnected,
    ExpansionWorkerStateRpcActive,
} ExpansionWorkerState;

typedef enum {
    ExpansionWorkerExitReasonUnknown,
    ExpansionWorkerExitReasonUser,
    ExpansionWorkerExitReasonError,
    ExpansionWorkerExitReasonTimeout,
} ExpansionWorkerExitReason;

typedef enum {
    ExpansionWorkerFlagStop = 1 << 0,
    ExpansionWorkerFlagData = 1 << 1,
    ExpansionWorkerFlagError = 1 << 2,
} ExpansionWorkerFlag;

#define EXPANSION_ALL_FLAGS \
    (ExpansionWorkerFlagData | ExpansionWorkerFlagStop | ExpansionWorkerFlagError)

struct ExpansionWorker {
    FuriThread* thread;
    FuriStreamBuffer* rx_buf;
    FuriSemaphore* tx_semaphore;

    FuriHalSerialId serial_id;
    FuriHalSerialHandle* serial_handle;

    RpcSession* rpc_session;

    ExpansionWorkerState state;
    ExpansionWorkerExitReason exit_reason;
    ExpansionWorkerCallback callback;
    void* cb_context;
};

// Called in UART IRQ context
static void expansion_worker_serial_rx_callback(
    FuriHalSerialHandle* handle,
    FuriHalSerialRxEvent event,
    void* context) {
    furi_assert(handle);
    furi_assert(context);

    ExpansionWorker* instance = context;

    if(event & (FuriHalSerialRxEventNoiseError | FuriHalSerialRxEventFrameError |
                FuriHalSerialRxEventOverrunError)) {
        furi_thread_flags_set(furi_thread_get_id(instance->thread), ExpansionWorkerFlagError);
    } else if(event & FuriHalSerialRxEventData) {
        while(furi_hal_serial_async_rx_available(handle)) {
            const uint8_t data = furi_hal_serial_async_rx(handle);
            furi_stream_buffer_send(instance->rx_buf, &data, sizeof(data), 0);
        }
        furi_thread_flags_set(furi_thread_get_id(instance->thread), ExpansionWorkerFlagData);
    }
}

static size_t expansion_worker_receive_callback(uint8_t* data, size_t data_size, void* context) {
    ExpansionWorker* instance = context;

    size_t received_size = 0;

    while(true) {
        received_size += furi_stream_buffer_receive(
            instance->rx_buf, data + received_size, data_size - received_size, 0);

        if(received_size == data_size) break;

        const uint32_t flags = furi_thread_flags_wait(
            EXPANSION_ALL_FLAGS, FuriFlagWaitAny, furi_ms_to_ticks(EXPANSION_PROTOCOL_TIMEOUT_MS));

        if(flags & FuriFlagError) {
            if(flags == (unsigned)FuriFlagErrorTimeout) {
                // Exiting due to timeout
                instance->exit_reason = ExpansionWorkerExitReasonTimeout;
            } else {
                // Exiting due to an unspecified error
                instance->exit_reason = ExpansionWorkerExitReasonError;
            }
            break;
        } else if(flags & ExpansionWorkerFlagStop) {
            // Exiting due to explicit request
            instance->exit_reason = ExpansionWorkerExitReasonUser;
            break;
        } else if(flags & ExpansionWorkerFlagError) {
            // Exiting due to RPC error
            instance->exit_reason = ExpansionWorkerExitReasonError;
            break;
        } else if(flags & ExpansionWorkerFlagData) {
            // Go to buffer reading
            continue;
        }
    }

    return received_size;
}

static inline bool
    expansion_worker_receive_frame(ExpansionWorker* instance, ExpansionFrame* frame) {
    return expansion_protocol_decode(frame, expansion_worker_receive_callback, instance) ==
           ExpansionProtocolStatusOk;
}

static size_t
    expansion_worker_send_callback(const uint8_t* data, size_t data_size, void* context) {
    ExpansionWorker* instance = context;
    furi_hal_serial_tx(instance->serial_handle, data, data_size);
    furi_hal_serial_tx_wait_complete(instance->serial_handle);
    return data_size;
}

static inline bool
    expansion_worker_send_frame(ExpansionWorker* instance, const ExpansionFrame* frame) {
    return expansion_protocol_encode(frame, expansion_worker_send_callback, instance) ==
           ExpansionProtocolStatusOk;
}

static bool expansion_worker_send_heartbeat(ExpansionWorker* instance) {
    const ExpansionFrame frame = {
        .header.type = ExpansionFrameTypeHeartbeat,
        .content.heartbeat = {},
    };

    return expansion_worker_send_frame(instance, &frame);
}

static bool
    expansion_worker_send_status_response(ExpansionWorker* instance, ExpansionFrameError error) {
    const ExpansionFrame frame = {
        .header.type = ExpansionFrameTypeStatus,
        .content.status.error = error,
    };

    return expansion_worker_send_frame(instance, &frame);
}

static bool expansion_worker_send_data_response(
    ExpansionWorker* instance,
    const uint8_t* data,
    size_t data_size) {
    furi_assert(data_size <= EXPANSION_PROTOCOL_MAX_DATA_SIZE);

    ExpansionFrame frame = {
        .header.type = ExpansionFrameTypeData,
        .content.data.size = data_size,
    };

    memcpy(frame.content.data.bytes, data, data_size);
    return expansion_worker_send_frame(instance, &frame);
}

// Called in Rpc session thread context
static void expansion_worker_rpc_send_callback(void* context, uint8_t* data, size_t data_size) {
    ExpansionWorker* instance = context;

    for(size_t sent_data_size = 0; sent_data_size < data_size;) {
        if(furi_semaphore_acquire(
               instance->tx_semaphore, furi_ms_to_ticks(EXPANSION_PROTOCOL_TIMEOUT_MS)) !=
           FuriStatusOk) {
            furi_thread_flags_set(furi_thread_get_id(instance->thread), ExpansionWorkerFlagError);
            break;
        }

        const size_t current_data_size =
            MIN(data_size - sent_data_size, EXPANSION_PROTOCOL_MAX_DATA_SIZE);
        if(!expansion_worker_send_data_response(
               instance, data + sent_data_size, current_data_size)) {
            furi_semaphore_release(instance->tx_semaphore);
            break;
        }
        sent_data_size += current_data_size;
    }
}

static bool expansion_worker_rpc_session_open(ExpansionWorker* instance) {
    Rpc* rpc = furi_record_open(RECORD_RPC);
    instance->rpc_session = rpc_session_open(rpc, RpcOwnerUart);

    if(instance->rpc_session) {
        instance->tx_semaphore = furi_semaphore_alloc(1, 1);
        rpc_session_set_context(instance->rpc_session, instance);
        rpc_session_set_send_bytes_callback(
            instance->rpc_session, expansion_worker_rpc_send_callback);
    }

    return instance->rpc_session != NULL;
}

static void expansion_worker_rpc_session_close(ExpansionWorker* instance) {
    if(instance->rpc_session) {
        rpc_session_close(instance->rpc_session);
        furi_semaphore_free(instance->tx_semaphore);
    }

    furi_record_close(RECORD_RPC);
}

static bool expansion_worker_handle_state_handshake(
    ExpansionWorker* instance,
    const ExpansionFrame* rx_frame) {
    bool success = false;

    do {
        if(rx_frame->header.type != ExpansionFrameTypeBaudRate) break;
        const uint32_t baud_rate = rx_frame->content.baud_rate.baud;

        FURI_LOG_D(TAG, "Proposed baud rate: %lu", baud_rate);

        if(furi_hal_serial_is_baud_rate_supported(instance->serial_handle, baud_rate)) {
            instance->state = ExpansionWorkerStateConnected;
            instance->callback(instance->cb_context, ExpansionWorkerCallbackReasonConnected);
            // Send response at previous baud rate
            if(!expansion_worker_send_status_response(instance, ExpansionFrameErrorNone)) break;
            furi_hal_serial_set_br(instance->serial_handle, baud_rate);

        } else {
            if(!expansion_worker_send_status_response(instance, ExpansionFrameErrorBaudRate))
                break;
            FURI_LOG_E(TAG, "Bad baud rate");
        }
        success = true;
    } while(false);

    return success;
}

static bool expansion_worker_handle_state_connected(
    ExpansionWorker* instance,
    const ExpansionFrame* rx_frame) {
    bool success = false;

    do {
        if(rx_frame->header.type == ExpansionFrameTypeControl) {
            const uint8_t command = rx_frame->content.control.command;
            if(command == ExpansionFrameControlCommandStartRpc) {
                if(!expansion_worker_rpc_session_open(instance)) break;
                instance->state = ExpansionWorkerStateRpcActive;
            } else if(command == ExpansionFrameControlCommandEnableOtg) {
                Power* power = furi_record_open(RECORD_POWER);
                power_enable_otg(power, true);
                furi_record_close(RECORD_POWER);
            } else if(command == ExpansionFrameControlCommandDisableOtg) {
                Power* power = furi_record_open(RECORD_POWER);
                power_enable_otg(power, false);
                furi_record_close(RECORD_POWER);
            } else {
                break;
            }

            if(!expansion_worker_send_status_response(instance, ExpansionFrameErrorNone)) break;

        } else if(rx_frame->header.type == ExpansionFrameTypeHeartbeat) {
            if(!expansion_worker_send_heartbeat(instance)) break;

        } else {
            break;
        }
        success = true;
    } while(false);

    return success;
}

static bool expansion_worker_handle_state_rpc_active(
    ExpansionWorker* instance,
    const ExpansionFrame* rx_frame) {
    bool success = false;

    do {
        if(rx_frame->header.type == ExpansionFrameTypeData) {
            if(!expansion_worker_send_status_response(instance, ExpansionFrameErrorNone)) break;

            const size_t size_consumed = rpc_session_feed(
                instance->rpc_session,
                rx_frame->content.data.bytes,
                rx_frame->content.data.size,
                EXPANSION_PROTOCOL_TIMEOUT_MS);
            if(size_consumed != rx_frame->content.data.size) break;

        } else if(rx_frame->header.type == ExpansionFrameTypeControl) {
            const uint8_t command = rx_frame->content.control.command;
            if(command == ExpansionFrameControlCommandStopRpc) {
                instance->state = ExpansionWorkerStateConnected;
                expansion_worker_rpc_session_close(instance);
            } else {
                break;
            }

            if(!expansion_worker_send_status_response(instance, ExpansionFrameErrorNone)) break;

        } else if(rx_frame->header.type == ExpansionFrameTypeStatus) {
            if(rx_frame->content.status.error != ExpansionFrameErrorNone) break;
            furi_semaphore_release(instance->tx_semaphore);

        } else if(rx_frame->header.type == ExpansionFrameTypeHeartbeat) {
            if(!expansion_worker_send_heartbeat(instance)) break;

        } else {
            break;
        }
        success = true;
    } while(false);

    return success;
}

typedef bool (*ExpansionWorkerStateHandler)(ExpansionWorker*, const ExpansionFrame*);

static const ExpansionWorkerStateHandler expansion_handlers[] = {
    [ExpansionWorkerStateHandShake] = expansion_worker_handle_state_handshake,
    [ExpansionWorkerStateConnected] = expansion_worker_handle_state_connected,
    [ExpansionWorkerStateRpcActive] = expansion_worker_handle_state_rpc_active,
};

static inline void expansion_worker_state_machine(ExpansionWorker* instance) {
    ExpansionFrame rx_frame;

    while(true) {
        if(!expansion_worker_receive_frame(instance, &rx_frame)) break;
        if(!expansion_handlers[instance->state](instance, &rx_frame)) break;
    }
}

static int32_t expansion_worker(void* context) {
    furi_assert(context);
    ExpansionWorker* instance = context;

    furi_hal_power_insomnia_enter();

    instance->serial_handle = furi_hal_serial_control_acquire(instance->serial_id);
    furi_check(instance->serial_handle);

    FURI_LOG_D(TAG, "Worker started");

    instance->state = ExpansionWorkerStateHandShake;
    instance->exit_reason = ExpansionWorkerExitReasonUnknown;

    furi_hal_serial_init(instance->serial_handle, EXPANSION_PROTOCOL_DEFAULT_BAUD_RATE);

    furi_hal_serial_async_rx_start(
        instance->serial_handle, expansion_worker_serial_rx_callback, instance, true);

    if(expansion_worker_send_heartbeat(instance)) {
        expansion_worker_state_machine(instance);
    }

    furi_hal_serial_async_rx_stop(instance->serial_handle);

    if(instance->state == ExpansionWorkerStateRpcActive) {
        expansion_worker_rpc_session_close(instance);
    }

    FURI_LOG_D(TAG, "Worker stopped");

    furi_hal_serial_control_release(instance->serial_handle);
    furi_hal_power_insomnia_exit();

    // Do not invoke worker callback on user-requested exit
    if((instance->exit_reason != ExpansionWorkerExitReasonUser) && (instance->callback != NULL)) {
        instance->callback(instance->cb_context, ExpansionWorkerCallbackReasonExit);
    }

    return 0;
}

ExpansionWorker* expansion_worker_alloc(FuriHalSerialId serial_id) {
    ExpansionWorker* instance = malloc(sizeof(ExpansionWorker));

    instance->thread = furi_thread_alloc_ex(
        TAG "Worker", EXPANSION_WORKER_STACK_SZIE, expansion_worker, instance);
    instance->rx_buf = furi_stream_buffer_alloc(EXPANSION_WORKER_BUFFER_SIZE, 1);
    instance->serial_id = serial_id;

    // Improves responsiveness in heavy games at the expense of dropped frames
    furi_thread_set_priority(instance->thread, FuriThreadPriorityLow);

    return instance;
}

void expansion_worker_free(ExpansionWorker* instance) {
    furi_stream_buffer_free(instance->rx_buf);
    furi_thread_join(instance->thread);
    furi_thread_free(instance->thread);
    free(instance);
}

void expansion_worker_set_callback(
    ExpansionWorker* instance,
    ExpansionWorkerCallback callback,
    void* context) {
    instance->callback = callback;
    instance->cb_context = context;
}

void expansion_worker_start(ExpansionWorker* instance) {
    furi_thread_start(instance->thread);
}

void expansion_worker_stop(ExpansionWorker* instance) {
    furi_thread_flags_set(furi_thread_get_id(instance->thread), ExpansionWorkerFlagStop);
    furi_thread_join(instance->thread);
}

#endif
