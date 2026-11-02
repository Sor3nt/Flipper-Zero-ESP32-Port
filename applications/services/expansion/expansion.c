#include "expansion.h"

#ifdef ESP_PLATFORM
#include <boards/board.h>
#endif

#if defined(ESP_PLATFORM) && defined(BOARD_HAS_EXPANSION_UART) && BOARD_HAS_EXPANSION_UART

#include <driver/uart.h>
#include <furi.h>
#include <furi_hal_uart_shared.h>
#include <furi_hal_power.h>
#include <storage/storage.h>
#include <toolbox/api_lock.h>

#include "expansion_worker.h"
#include "expansion_settings.h"

#define TAG "ExpansionSrv"
#define EXPANSION_CONTROL_QUEUE_SIZE (8UL)
#define EXPANSION_CONTROL_STACK_SIZE (2048UL)
#define EXPANSION_DETECT_READ_TIMEOUT_MS (50U)
#define EXPANSION_DETECT_RX_BUFFER_SIZE (256U)
#define EXPANSION_DETECT_TX_BUFFER_SIZE (0U)
#define EXPANSION_DETECT_UART BOARD_EXPANSION_UART

typedef enum {
    ExpansionStateDisabled,
    ExpansionStateEnabled,
    ExpansionStateRunning,
    ExpansionStateConnectionEstablished,
} ExpansionState;

typedef enum {
    ExpansionMessageTypeEnable,
    ExpansionMessageTypeDisable,
    ExpansionMessageTypeSetListenSerial,
    ExpansionMessageTypeReloadSettings,
    ExpansionMessageTypeModuleConnected,
    ExpansionMessageTypeModuleDisconnected,
    ExpansionMessageTypeListenerFailed,
    ExpansionMessageTypeConnectionEstablished,
    ExpansionMessageTypeIsConnected,
    ExpansionMessageTypeIdle,
} ExpansionMessageType;

typedef union {
    FuriHalSerialId serial_id;
    bool* is_connected;
    bool idle;
} ExpansionMessageData;

typedef struct {
    ExpansionMessageType type;
    ExpansionMessageData data;
    FuriApiLock api_lock;
} ExpansionMessage;

struct Expansion {
    FuriThread* thread;
    FuriThread* detector_thread;
    FuriMessageQueue* queue;
    FuriHalSerialId serial_id;
    ExpansionWorker* worker;
    ExpansionState state;
    volatile bool detector_stop;
    /* Light-sleep idle: the listener (UART RX polled every 50 ms, light sleep
     * inhibited) is parked and must not be started until the device wakes. */
    bool idle;
};

static bool expansion_queue_message(Expansion* instance, ExpansionMessageType type) {
    ExpansionMessage message = {
        .type = type,
        .api_lock = NULL,
    };
    while(!instance->detector_stop) {
        if(furi_message_queue_put(instance->queue, &message, 0) == FuriStatusOk) return true;
        furi_delay_ms(10);
    }
    return false;
}

static bool expansion_detect_callback(void* context) {
    Expansion* instance = context;
    return expansion_queue_message(instance, ExpansionMessageTypeModuleConnected);
}

static void expansion_worker_callback(void* context, ExpansionWorkerCallbackReason reason) {
    Expansion* instance = context;
    ExpansionMessage message = {
        .type = reason == ExpansionWorkerCallbackReasonExit ?
                    ExpansionMessageTypeModuleDisconnected :
                    ExpansionMessageTypeConnectionEstablished,
        .api_lock = NULL,
    };
    if(reason == ExpansionWorkerCallbackReasonConnected) {
        message.api_lock = api_lock_alloc_locked();
    }
    const FuriStatus status =
        furi_message_queue_put(instance->queue, &message, FuriWaitForever);
    furi_check(status == FuriStatusOk);
}

static int32_t expansion_detect_thread(void* context) {
    Expansion* instance = context;
    const uart_port_t uart_port = EXPANSION_DETECT_UART;
    while(!instance->detector_stop) {
        bool uart_claimed = false;
        bool uart_installed = false;
        bool sleep_inhibited = false;
        bool module_detected = false;
        bool preempted = false;
        bool listener_failed = false;
        esp_err_t err = ESP_OK;

        while(!instance->detector_stop) {
            if(furi_hal_uart_shared_preemption_requested()) {
                furi_delay_ms(10);
                continue;
            }
            const FuriHalUartSharedAcquireResult result =
                furi_hal_uart_shared_acquire_background(100);
            if(result == FuriHalUartSharedAcquireAcquired) {
                uart_claimed = true;
                break;
            }
            if(result == FuriHalUartSharedAcquireError) {
                FURI_LOG_E(TAG, "Could not route console away from Expansion UART");
                listener_failed = true;
                break;
            }
        }
        if(!uart_claimed) {
            if(listener_failed) {
                (void)expansion_queue_message(instance, ExpansionMessageTypeListenerFailed);
                return 0;
            }
            continue;
        }

        furi_hal_power_light_sleep_inhibit_enter();
        sleep_inhibited = true;
        err = uart_driver_install(
            uart_port,
            EXPANSION_DETECT_RX_BUFFER_SIZE,
            EXPANSION_DETECT_TX_BUFFER_SIZE,
            0,
            NULL,
            0);
        if(err == ESP_OK) {
            uart_installed = true;
            err = uart_param_config(uart_port, &(uart_config_t){
                .baud_rate = BOARD_EXPANSION_UART_BAUD,
                .data_bits = UART_DATA_8_BITS,
                .parity = UART_PARITY_DISABLE,
                .stop_bits = UART_STOP_BITS_1,
                .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                .source_clk = UART_SCLK_DEFAULT,
            });
        }
        if(err == ESP_OK) {
            err = uart_set_pin(
                uart_port,
                BOARD_PIN_EXPANSION_TX,
                BOARD_PIN_EXPANSION_RX,
                UART_PIN_NO_CHANGE,
                UART_PIN_NO_CHANGE);
        }
        if(err != ESP_OK) {
            FURI_LOG_E(TAG, "Expansion listener UART setup failed: %s", esp_err_to_name(err));
            listener_failed = true;
        } else {
            while(!instance->detector_stop) {
                if(furi_hal_uart_shared_preemption_requested()) {
                    preempted = true;
                    break;
                }
                uint8_t incoming_byte;
                const int count = uart_read_bytes(
                    uart_port,
                    &incoming_byte,
                    sizeof(incoming_byte),
                    pdMS_TO_TICKS(EXPANSION_DETECT_READ_TIMEOUT_MS));
                if(count < 0) {
                    FURI_LOG_E(TAG, "Expansion listener UART read failed");
                    listener_failed = true;
                    break;
                }
                if(count > 0) {
                    module_detected = true;
                    break;
                }
            }
        }

        bool uart_safe_to_release = true;
        if(uart_installed) {
            err = uart_driver_delete(uart_port);
            if(err != ESP_OK) {
                FURI_LOG_E(TAG, "Could not remove Expansion listener UART: %s", esp_err_to_name(err));
                uart_safe_to_release = false;
                listener_failed = true;
            }
        }
        if(uart_claimed && uart_safe_to_release) {
            if(furi_hal_uart_shared_release()) {
                uart_claimed = false;
            } else {
                FURI_LOG_E(TAG, "Could not release shared UART after Expansion detection");
                uart_safe_to_release = false;
                listener_failed = true;
            }
        }
        if(sleep_inhibited && uart_safe_to_release) {
            furi_hal_power_light_sleep_inhibit_exit();
            sleep_inhibited = false;
        }
        if(listener_failed) {
            (void)expansion_queue_message(instance, ExpansionMessageTypeListenerFailed);
            return 0;
        }
        if(module_detected && !instance->detector_stop) {
            (void)expansion_detect_callback(instance);
            return 0;
        }
        if(!preempted) return 0;
    }
    return 0;
}

static bool expansion_listener_start(Expansion* instance) {
    furi_assert(instance->detector_thread == NULL);
    if(instance->idle) return true; /* started by the wake-up handler instead */
    instance->detector_stop = false;
    instance->detector_thread =
        furi_thread_alloc_ex("ExpansionDetect", 2048, expansion_detect_thread, instance);
    if(!instance->detector_thread) {
        FURI_LOG_E(TAG, "Could not allocate Expansion listener");
        return false;
    }
    furi_thread_start(instance->detector_thread);
    return true;
}

static void expansion_listener_stop(Expansion* instance) {
    if(!instance->detector_thread) return;
    instance->detector_stop = true;
    furi_thread_join(instance->detector_thread);
    furi_thread_free(instance->detector_thread);
    instance->detector_thread = NULL;
}

static void expansion_control_handler_enable(Expansion* instance) {
    if(instance->state != ExpansionStateDisabled) return;

    ExpansionSettings settings;
    expansion_settings_load(&settings);
    if(settings.uart_index != FuriHalSerialIdUsart) {
        FURI_LOG_I(TAG, "Expansion listener disabled by settings");
        return;
    }

    instance->serial_id = FuriHalSerialIdUsart;
    instance->state = ExpansionStateEnabled;
    if(!expansion_listener_start(instance)) {
        instance->state = ExpansionStateDisabled;
        return;
    }
    FURI_LOG_I(
        TAG,
        "Listening for Expansion modules on UART%d (TX=GPIO%u RX=GPIO%u)",
        (int)EXPANSION_DETECT_UART,
        (unsigned)BOARD_PIN_EXPANSION_TX,
        (unsigned)BOARD_PIN_EXPANSION_RX);
}

static void expansion_control_handler_disable(Expansion* instance) {
    if(instance->state == ExpansionStateDisabled) return;

    if(instance->state == ExpansionStateRunning ||
       instance->state == ExpansionStateConnectionEstablished) {
        expansion_worker_stop(instance->worker);
        expansion_worker_free(instance->worker);
        instance->worker = NULL;
    } else {
        expansion_listener_stop(instance);
    }
    instance->state = ExpansionStateDisabled;
}

static void expansion_control_handler_set_listen_serial(
    Expansion* instance,
    FuriHalSerialId serial_id) {
    if(serial_id != FuriHalSerialIdUsart) {
        expansion_control_handler_disable(instance);
        return;
    }
    if(instance->state != ExpansionStateDisabled && instance->serial_id == serial_id) return;
    expansion_control_handler_disable(instance);
    instance->state = ExpansionStateDisabled;
    instance->serial_id = serial_id;
    instance->state = ExpansionStateEnabled;
    if(!expansion_listener_start(instance)) instance->state = ExpansionStateDisabled;
}

/* Light sleep granted (idle) or left again. Only the passive listener is parked;
 * a connected module (worker) keeps its own light-sleep inhibit. The listener
 * restarts on wake, so a module plugged in meanwhile is detected then. */
static void expansion_control_handler_idle(Expansion* instance, bool idle) {
    if(instance->idle == idle) return;
    instance->idle = idle;
    if(instance->state != ExpansionStateEnabled) return;
    expansion_listener_stop(instance); /* also reaps a detector that ended by itself */
    if(!idle && !expansion_listener_start(instance)) instance->state = ExpansionStateDisabled;
}

static void expansion_control_handler_module_connected(Expansion* instance) {
    if(instance->state != ExpansionStateEnabled) return;
    expansion_listener_stop(instance);
    if(instance->idle) return; /* detected on the way into sleep: re-detected on wake */
    instance->worker = expansion_worker_alloc(instance->serial_id);
    if(!instance->worker) {
        FURI_LOG_E(TAG, "Could not allocate Expansion worker");
        expansion_listener_start(instance);
        return;
    }
    instance->state = ExpansionStateRunning;
    expansion_worker_set_callback(instance->worker, expansion_worker_callback, instance);
    expansion_worker_start(instance->worker);
}

static void expansion_control_handler_module_disconnected(Expansion* instance) {
    if(instance->state != ExpansionStateRunning &&
       instance->state != ExpansionStateConnectionEstablished) return;
    expansion_worker_free(instance->worker);
    instance->worker = NULL;
    instance->state = ExpansionStateEnabled;
    if(!expansion_listener_start(instance)) instance->state = ExpansionStateDisabled;
}

static void expansion_control_handler_listener_failed(Expansion* instance) {
    if(instance->state != ExpansionStateEnabled) return;
    expansion_listener_stop(instance);
    instance->state = ExpansionStateDisabled;
    FURI_LOG_E(TAG, "Expansion listener stopped after a UART error");
}

static int32_t expansion_control(void* context) {
    Expansion* instance = context;
    for(;;) {
        ExpansionMessage message;
        furi_check(
            furi_message_queue_get(instance->queue, &message, FuriWaitForever) == FuriStatusOk);

        switch(message.type) {
        case ExpansionMessageTypeEnable:
            expansion_control_handler_enable(instance);
            break;
        case ExpansionMessageTypeDisable:
            expansion_control_handler_disable(instance);
            break;
        case ExpansionMessageTypeSetListenSerial:
            expansion_control_handler_set_listen_serial(instance, message.data.serial_id);
            break;
        case ExpansionMessageTypeReloadSettings:
            expansion_control_handler_disable(instance);
            expansion_control_handler_enable(instance);
            break;
        case ExpansionMessageTypeModuleConnected:
            expansion_control_handler_module_connected(instance);
            break;
        case ExpansionMessageTypeModuleDisconnected:
            expansion_control_handler_module_disconnected(instance);
            break;
        case ExpansionMessageTypeListenerFailed:
            expansion_control_handler_listener_failed(instance);
            break;
        case ExpansionMessageTypeConnectionEstablished:
            if(instance->state == ExpansionStateRunning) {
                instance->state = ExpansionStateConnectionEstablished;
            }
            break;
        case ExpansionMessageTypeIsConnected:
            *message.data.is_connected =
                instance->state == ExpansionStateConnectionEstablished;
            break;
        case ExpansionMessageTypeIdle:
            expansion_control_handler_idle(instance, message.data.idle);
            break;
        }

        if(message.api_lock) api_lock_unlock(message.api_lock);
    }
    return 0;
}

static Expansion* expansion_alloc(void) {
    Expansion* instance = calloc(1, sizeof(Expansion));
    furi_check(instance);
    instance->queue =
        furi_message_queue_alloc(EXPANSION_CONTROL_QUEUE_SIZE, sizeof(ExpansionMessage));
    instance->thread =
        furi_thread_alloc_ex(TAG, EXPANSION_CONTROL_STACK_SIZE, expansion_control, instance);
    furi_check(instance->queue);
    furi_check(instance->thread);
    return instance;
}

static void expansion_storage_callback(const void* message, void* context) {
    const StorageEvent* event = message;
    Expansion* instance = context;
    if(event->type == StorageEventTypeCardMount) {
        ExpansionMessage reload = {
            .type = ExpansionMessageTypeReloadSettings,
            .api_lock = NULL,
        };
        furi_check(furi_message_queue_put(instance->queue, &reload, FuriWaitForever) == FuriStatusOk);
    }
}

/* Power-service thread: hand the change over to the expansion thread. */
static void expansion_power_idle_callback(const void* message, void* context) {
    const FuriHalPowerIdleEvent* event = message;
    Expansion* instance = context;
    ExpansionMessage idle = {
        .type = ExpansionMessageTypeIdle,
        .data.idle = event->idle,
        .api_lock = NULL,
    };
    furi_check(furi_message_queue_put(instance->queue, &idle, FuriWaitForever) == FuriStatusOk);
}

void expansion_on_system_start(void* arg) {
    UNUSED(arg);
    Expansion* instance = expansion_alloc();
    furi_record_create(RECORD_EXPANSION, instance);
    furi_thread_start(instance->thread);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    furi_pubsub_subscribe(storage_get_pubsub(storage), expansion_storage_callback, instance);

    FuriPubSub* idle_pubsub = furi_hal_power_get_idle_pubsub();
    if(idle_pubsub) furi_pubsub_subscribe(idle_pubsub, expansion_power_idle_callback, instance);

    expansion_enable(instance);
}

static void expansion_send_sync(Expansion* instance, ExpansionMessage* message) {
    message->api_lock = api_lock_alloc_locked();
    furi_check(furi_message_queue_put(instance->queue, message, FuriWaitForever) == FuriStatusOk);
    api_lock_wait_unlock_and_free(message->api_lock);
}

void expansion_enable(Expansion* instance) {
    furi_assert(instance);
    ExpansionMessage message = {.type = ExpansionMessageTypeEnable};
    expansion_send_sync(instance, &message);
}

void expansion_disable(Expansion* instance) {
    furi_assert(instance);
    ExpansionMessage message = {.type = ExpansionMessageTypeDisable};
    expansion_send_sync(instance, &message);
}

bool expansion_is_connected(Expansion* instance) {
    furi_assert(instance);
    bool connected = false;
    ExpansionMessage message = {
        .type = ExpansionMessageTypeIsConnected,
        .data.is_connected = &connected,
    };
    expansion_send_sync(instance, &message);
    return connected;
}

void expansion_set_listen_serial(Expansion* instance, FuriHalSerialId serial_id) {
    furi_assert(instance);
    furi_check(serial_id == FuriHalSerialIdUsart);
    ExpansionMessage message = {
        .type = ExpansionMessageTypeSetListenSerial,
        .data.serial_id = serial_id,
    };
    expansion_send_sync(instance, &message);
}

#elif defined(ESP_PLATFORM)

#include <furi.h>

struct Expansion {
    uint8_t unused;
};

static Expansion expansion_instance;

void expansion_on_system_start(void* arg) {
    UNUSED(arg);
    FURI_LOG_W("ExpansionSrv", "Expansion serial is not configured for this ESP32 board");
    furi_record_create(RECORD_EXPANSION, &expansion_instance);
}

void expansion_enable(Expansion* instance) {
    furi_check(instance);
}

void expansion_disable(Expansion* instance) {
    furi_check(instance);
}

bool expansion_is_connected(Expansion* instance) {
    furi_check(instance);
    return false;
}

void expansion_set_listen_serial(Expansion* instance, FuriHalSerialId serial_id) {
    furi_check(instance);
    furi_check(serial_id < FuriHalSerialIdMax);
}

#else

#include <furi_hal_serial_control.h>

#include <furi.h>
#include <storage/storage.h>
#include <toolbox/api_lock.h>

#include "expansion_worker.h"
#include "expansion_settings.h"

#define TAG "ExpansionSrv"

#define EXPANSION_CONTROL_QUEUE_SIZE (8UL)
#define EXPANSION_CONTROL_STACK_SIZE (768UL)

typedef enum {
    ExpansionStateDisabled,
    ExpansionStateEnabled,
    ExpansionStateRunning,
    ExpansionStateConnectionEstablished,
} ExpansionState;

typedef enum {
    ExpansionMessageTypeEnable,
    ExpansionMessageTypeDisable,
    ExpansionMessageTypeSetListenSerial,
    ExpansionMessageTypeReloadSettings,
    ExpansionMessageTypeModuleConnected,
    ExpansionMessageTypeModuleDisconnected,
    ExpansionMessageTypeConnectionEstablished,
    ExpansionMessageTypeIsConnected,
} ExpansionMessageType;

typedef union {
    FuriHalSerialId serial_id;
    bool* is_connected;
} ExpansionMessageData;

typedef struct {
    ExpansionMessageType type;
    ExpansionMessageData data;
    FuriApiLock api_lock;
} ExpansionMessage;

struct Expansion {
    FuriThread* thread;
    FuriMessageQueue* queue;
    FuriHalSerialId serial_id;
    ExpansionWorker* worker;
    ExpansionState state;
};

static const char* const expansion_uart_names[] = {
    "USART",
    "LPUART",
};

// Called from the serial control thread
static void expansion_detect_callback(void* context) {
    furi_assert(context);
    Expansion* instance = context;

    ExpansionMessage message = {
        .type = ExpansionMessageTypeModuleConnected,
        .api_lock = NULL, // Not locking the API here to avoid a deadlock
    };

    // Not waiting for available queue space, discarding message if there is none
    const FuriStatus status = furi_message_queue_put(instance->queue, &message, 0);
    UNUSED(status);
}

static void expansion_worker_callback(void* context, ExpansionWorkerCallbackReason reason) {
    furi_assert(context);
    Expansion* instance = context;

    ExpansionMessage message;
    switch(reason) {
    case ExpansionWorkerCallbackReasonExit:
        message.type = ExpansionMessageTypeModuleDisconnected;
        message.api_lock = NULL; // Not locking the API here to avoid a deadlock
        break;

    case ExpansionWorkerCallbackReasonConnected:
        message.type = ExpansionMessageTypeConnectionEstablished;
        message.api_lock = api_lock_alloc_locked();
        break;
    };

    const FuriStatus status = furi_message_queue_put(instance->queue, &message, FuriWaitForever);
    furi_check(status == FuriStatusOk);
}

static void
    expansion_control_handler_enable(Expansion* instance, const ExpansionMessageData* data) {
    UNUSED(data);

    if(instance->state != ExpansionStateDisabled) {
        return;
    }

    ExpansionSettings settings;
    expansion_settings_load(&settings);

    if(settings.uart_index < FuriHalSerialIdMax) {
        instance->state = ExpansionStateEnabled;
        instance->serial_id = settings.uart_index;
        furi_hal_serial_control_set_expansion_callback(
            instance->serial_id, expansion_detect_callback, instance);

        FURI_LOG_D(TAG, "Detection enabled on %s", expansion_uart_names[instance->serial_id]);
    }
}

static void
    expansion_control_handler_disable(Expansion* instance, const ExpansionMessageData* data) {
    UNUSED(data);
    if(instance->state == ExpansionStateDisabled) {
        return;
    } else if(
        instance->state == ExpansionStateRunning ||
        instance->state == ExpansionStateConnectionEstablished) {
        expansion_worker_stop(instance->worker);
        expansion_worker_free(instance->worker);
    } else {
        furi_hal_serial_control_set_expansion_callback(instance->serial_id, NULL, NULL);
    }

    instance->state = ExpansionStateDisabled;

    FURI_LOG_D(TAG, "Detection disabled");
}

static void expansion_control_handler_set_listen_serial(
    Expansion* instance,
    const ExpansionMessageData* data) {
    if(instance->state != ExpansionStateDisabled && instance->serial_id == data->serial_id) {
        return;

    } else if(
        instance->state == ExpansionStateRunning ||
        instance->state == ExpansionStateConnectionEstablished) {
        expansion_worker_stop(instance->worker);
        expansion_worker_free(instance->worker);

    } else if(instance->state == ExpansionStateEnabled) {
        furi_hal_serial_control_set_expansion_callback(instance->serial_id, NULL, NULL);
    }

    instance->state = ExpansionStateEnabled;
    instance->serial_id = data->serial_id;

    furi_hal_serial_control_set_expansion_callback(
        instance->serial_id, expansion_detect_callback, instance);

    FURI_LOG_D(TAG, "Listen serial changed to %s", expansion_uart_names[instance->serial_id]);
}

static void expansion_control_handler_reload_settings(
    Expansion* instance,
    const ExpansionMessageData* data) {
    UNUSED(data);

    ExpansionSettings settings;
    expansion_settings_load(&settings);

    if(settings.uart_index < FuriHalSerialIdMax) {
        const ExpansionMessageData data = {
            .serial_id = settings.uart_index,
        };

        expansion_control_handler_set_listen_serial(instance, &data);

    } else {
        expansion_control_handler_disable(instance, NULL);
    }
}

static void expansion_control_handler_module_connected(
    Expansion* instance,
    const ExpansionMessageData* data) {
    UNUSED(data);
    if(instance->state != ExpansionStateEnabled) {
        return;
    }

    furi_hal_serial_control_set_expansion_callback(instance->serial_id, NULL, NULL);

    instance->state = ExpansionStateRunning;
    instance->worker = expansion_worker_alloc(instance->serial_id);

    expansion_worker_set_callback(instance->worker, expansion_worker_callback, instance);
    expansion_worker_start(instance->worker);
}

static void expansion_control_handler_module_disconnected(
    Expansion* instance,
    const ExpansionMessageData* data) {
    UNUSED(data);
    if(instance->state != ExpansionStateRunning &&
       instance->state != ExpansionStateConnectionEstablished) {
        return;
    }

    instance->state = ExpansionStateEnabled;
    expansion_worker_free(instance->worker);
    furi_hal_serial_control_set_expansion_callback(
        instance->serial_id, expansion_detect_callback, instance);
}

static void expansion_control_handler_connection_established(
    Expansion* instance,
    const ExpansionMessageData* data) {
    UNUSED(data);
    if(instance->state != ExpansionStateRunning &&
       instance->state != ExpansionStateConnectionEstablished) {
        return;
    }

    instance->state = ExpansionStateConnectionEstablished;
}

static void
    expansion_control_handler_is_connected(Expansion* instance, const ExpansionMessageData* data) {
    *data->is_connected = instance->state == ExpansionStateConnectionEstablished;
}

typedef void (*ExpansionControlHandler)(Expansion*, const ExpansionMessageData*);

static const ExpansionControlHandler expansion_control_handlers[] = {
    [ExpansionMessageTypeEnable] = expansion_control_handler_enable,
    [ExpansionMessageTypeDisable] = expansion_control_handler_disable,
    [ExpansionMessageTypeSetListenSerial] = expansion_control_handler_set_listen_serial,
    [ExpansionMessageTypeReloadSettings] = expansion_control_handler_reload_settings,
    [ExpansionMessageTypeModuleConnected] = expansion_control_handler_module_connected,
    [ExpansionMessageTypeModuleDisconnected] = expansion_control_handler_module_disconnected,
    [ExpansionMessageTypeConnectionEstablished] = expansion_control_handler_connection_established,
    [ExpansionMessageTypeIsConnected] = expansion_control_handler_is_connected,
};

static int32_t expansion_control(void* context) {
    furi_assert(context);
    Expansion* instance = context;

    for(;;) {
        ExpansionMessage message;

        FuriStatus status = furi_message_queue_get(instance->queue, &message, FuriWaitForever);
        furi_check(status == FuriStatusOk);

        furi_check(message.type < COUNT_OF(expansion_control_handlers));
        expansion_control_handlers[message.type](instance, &message.data);

        if(message.api_lock != NULL) {
            api_lock_unlock(message.api_lock);
        }
    }

    return 0;
}

static Expansion* expansion_alloc(void) {
    Expansion* instance = malloc(sizeof(Expansion));

    instance->queue =
        furi_message_queue_alloc(EXPANSION_CONTROL_QUEUE_SIZE, sizeof(ExpansionMessage));
    instance->thread =
        furi_thread_alloc_ex(TAG, EXPANSION_CONTROL_STACK_SIZE, expansion_control, instance);

    return instance;
}

static void expansion_storage_callback(const void* message, void* context) {
    furi_assert(context);

    const StorageEvent* event = message;
    Expansion* instance = context;

    if(event->type == StorageEventTypeCardMount) {
        ExpansionMessage em = {
            .type = ExpansionMessageTypeReloadSettings,
            .api_lock = NULL,
        };

        furi_check(furi_message_queue_put(instance->queue, &em, FuriWaitForever) == FuriStatusOk);
    }
}

void expansion_on_system_start(void* arg) {
    UNUSED(arg);

    Expansion* instance = expansion_alloc();
    furi_record_create(RECORD_EXPANSION, instance);
    furi_thread_start(instance->thread);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    furi_pubsub_subscribe(storage_get_pubsub(storage), expansion_storage_callback, instance);

    if(storage_sd_status(storage) != FSE_OK) {
        FURI_LOG_D(TAG, "SD Card not ready, skipping settings");
        return;
    }

    expansion_enable(instance);
}

// Public API functions

void expansion_enable(Expansion* instance) {
    furi_check(instance);

    ExpansionMessage message = {
        .type = ExpansionMessageTypeEnable,
        .api_lock = api_lock_alloc_locked(),
    };

    furi_message_queue_put(instance->queue, &message, FuriWaitForever);
    api_lock_wait_unlock_and_free(message.api_lock);
}

void expansion_disable(Expansion* instance) {
    furi_check(instance);

    ExpansionMessage message = {
        .type = ExpansionMessageTypeDisable,
        .api_lock = api_lock_alloc_locked(),
    };

    furi_message_queue_put(instance->queue, &message, FuriWaitForever);
    api_lock_wait_unlock_and_free(message.api_lock);
}

bool expansion_is_connected(Expansion* instance) {
    furi_check(instance);
    bool is_connected;

    ExpansionMessage message = {
        .type = ExpansionMessageTypeIsConnected,
        .data.is_connected = &is_connected,
        .api_lock = api_lock_alloc_locked(),
    };

    furi_message_queue_put(instance->queue, &message, FuriWaitForever);
    api_lock_wait_unlock_and_free(message.api_lock);

    return is_connected;
}

void expansion_set_listen_serial(Expansion* instance, FuriHalSerialId serial_id) {
    furi_check(instance);
    furi_check(serial_id < FuriHalSerialIdMax);

    ExpansionMessage message = {
        .type = ExpansionMessageTypeSetListenSerial,
        .data.serial_id = serial_id,
        .api_lock = api_lock_alloc_locked(),
    };

    furi_message_queue_put(instance->queue, &message, FuriWaitForever);
    api_lock_wait_unlock_and_free(message.api_lock);
}

#endif
