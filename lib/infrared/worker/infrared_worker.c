#include "infrared_worker.h"

#include <furi_hal_infrared.h>
#include <furi_hal_power.h>
#include <float_tools.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>

#include <core/check.h>
#include <core/common_defines.h>

#include <notification/notification_messages.h>

#define INFRARED_WORKER_RX_TIMEOUT INFRARED_RAW_RX_TIMING_DELAY_US

#define INFRARED_WORKER_RX_RECEIVED         0x01
#define INFRARED_WORKER_RX_TIMEOUT_RECEIVED 0x02
#define INFRARED_WORKER_OVERRUN             0x04
#define INFRARED_WORKER_EXIT                0x08
#define INFRARED_WORKER_TX_FILL_BUFFER      0x10
#define INFRARED_WORKER_TX_MESSAGE_SENT     0x20
#define INFRARED_WORKER_TX_UNDERRUN         0x40
#define INFRARED_WORKER_TX_OVERFLOW         0x80
#define INFRARED_WORKER_RX_FRAME_END       0x100
#define INFRARED_WORKER_TX_ERROR            0x200
#define INFRARED_WORKER_RX_ERROR            0x400

#define INFRARED_WORKER_MIN_RAW_TIMINGS 8
#define INFRARED_WORKER_MIN_RAW_DURATION_US 2000

#define INFRARED_WORKER_ALL_RX_EVENTS                                    \
    (INFRARED_WORKER_RX_RECEIVED | INFRARED_WORKER_RX_TIMEOUT_RECEIVED | \
        INFRARED_WORKER_OVERRUN | INFRARED_WORKER_RX_FRAME_END | \
        INFRARED_WORKER_RX_ERROR | INFRARED_WORKER_EXIT)

#define INFRARED_WORKER_ALL_TX_EVENTS \
    (INFRARED_WORKER_TX_FILL_BUFFER | INFRARED_WORKER_TX_MESSAGE_SENT | \
    INFRARED_WORKER_TX_UNDERRUN | INFRARED_WORKER_TX_OVERFLOW | \
    INFRARED_WORKER_TX_ERROR | INFRARED_WORKER_EXIT)

#define INFRARED_WORKER_ALL_EVENTS (INFRARED_WORKER_ALL_RX_EVENTS | INFRARED_WORKER_ALL_TX_EVENTS)

typedef enum {
    InfraredWorkerStateIdle,
    InfraredWorkerStateRunRx,
    InfraredWorkerStateRunTx,
    InfraredWorkerStateWaitTxEnd,
    InfraredWorkerStateStopTx,
    InfraredWorkerStateStartTx,
} InfraredWorkerState;

struct InfraredWorkerSignal {
    bool decoded;
    size_t timings_cnt;
    union {
        InfraredMessage message;
        struct {
            /* +1 is for pause we add at the beginning */
            uint32_t timings[MAX_TIMINGS_AMOUNT + 1];
            uint32_t frequency;
            float duty_cycle;
        } raw;
    };

};

struct InfraredWorker {
    FuriThread* thread;
    /* Written by the RMT RX ISR, which also runs while the flash cache is off
     * (CONFIG_RMT_ISR_IRAM_SAFE): must be internal RAM. Allocated per RX session. */
    FuriStreamBuffer* rx_stream;
    /* Only touched from task context (worker thread and HAL TX task). */
    FuriStreamBuffer* tx_stream;

    InfraredWorkerSignal signal;
    InfraredWorkerState state;
    InfraredEncoderHandler* infrared_encoder;
    InfraredDecoderHandler* infrared_decoder;
    NotificationApp* notification;
    bool blink_enable;
    bool decode_enable;
    bool light_sleep_inhibited;

    /* Outside the rx/tx union: the owner may leave it unset (the FAP API does not
     * expose the setter), so alloc() must be able to leave it NULL for good. */
    InfraredWorkerRxErrorCallback rx_error_callback;
    void* rx_error_context;

    union {
        struct {
            InfraredWorkerGetSignalCallback get_signal_callback;
            InfraredWorkerMessageSentCallback message_sent_callback;
            void* get_signal_context;
            void* message_sent_context;
            uint32_t frequency;
            float duty_cycle;
            uint32_t tx_raw_cnt;
            bool need_reinitialization;
            bool steady_signal_sent;
            volatile bool underrun;
            volatile bool overflow;
            volatile bool error;
        } tx;
        struct {
            InfraredWorkerReceivedSignalCallback received_signal_callback;
            void* received_signal_context;
            volatile bool overrun;
            uint32_t frame_end_gap_us;
        } rx;
    };
};

static void infrared_worker_light_sleep_inhibit_begin(InfraredWorker* instance) {
    if(!instance->light_sleep_inhibited) {
        instance->light_sleep_inhibited = true;
        furi_hal_power_light_sleep_inhibit_enter();
    }
}

static void infrared_worker_light_sleep_inhibit_end(InfraredWorker* instance) {
    if(instance->light_sleep_inhibited) {
        instance->light_sleep_inhibited = false;
        furi_hal_power_light_sleep_inhibit_exit();
    }
}

typedef struct {
    uint32_t duration;
    bool level;
    FuriHalInfraredTxGetDataState state;
} InfraredWorkerTiming;

static int32_t infrared_worker_tx_thread(void* context);
static void infrared_worker_process_timings(
    InfraredWorker* instance,
    uint32_t duration,
    bool level);
static FuriHalInfraredTxGetDataState
    infrared_worker_furi_hal_data_isr_callback(void* context, uint32_t* duration, bool* level);
static void infrared_worker_furi_hal_message_sent_isr_callback(void* context);
static void infrared_worker_furi_hal_overflow_isr_callback(void* context);
static void infrared_worker_furi_hal_error_callback(void* context);

static bool IRAM_ATTR infrared_worker_rx_timeout_callback(void* context) {
    InfraredWorker* instance = context;
    return furi_thread_flags_set_from_isr(
        (FuriThreadId)instance->thread, INFRARED_WORKER_RX_TIMEOUT_RECEIVED);
}

static bool IRAM_ATTR infrared_worker_rx_callback(void* context, bool level, uint32_t duration) {
    InfraredWorker* instance = context;

    if(duration == 0) return false;
    LevelDuration level_duration = level_duration_make(level, duration);
    bool higher_priority_task_woken = false;

    size_t ret = furi_stream_buffer_send_from_isr(
        instance->rx_stream,
        &level_duration,
        sizeof(LevelDuration),
        &higher_priority_task_woken);
    uint32_t events = (ret == sizeof(LevelDuration)) ? INFRARED_WORKER_RX_RECEIVED :
                                                       INFRARED_WORKER_OVERRUN;
    if(ret != sizeof(LevelDuration)) {
        instance->rx.overrun = true;
    }

    higher_priority_task_woken |=
        furi_thread_flags_set_from_isr((FuriThreadId)instance->thread, events);
    return higher_priority_task_woken;
}

static bool IRAM_ATTR infrared_worker_rx_frame_end_callback(void* context, uint32_t gap_us) {
    InfraredWorker* instance = context;
    instance->rx.frame_end_gap_us = gap_us;
    return furi_thread_flags_set_from_isr(
        (FuriThreadId)instance->thread, INFRARED_WORKER_RX_FRAME_END);
}

static void infrared_worker_rx_error_callback(void* context) {
    InfraredWorker* instance = context;
    furi_thread_flags_set(furi_thread_get_id(instance->thread), INFRARED_WORKER_RX_ERROR);
}

static void infrared_worker_process_frame_end(InfraredWorker* instance) {
    /* An overrun invalidates the whole burst, including a frame-end event
     * that may have been delivered in the same notification batch. */
    if(instance->rx.overrun || !instance->decode_enable) return;

    const InfraredMessage* message_decoded =
        infrared_decode(
            instance->infrared_decoder, false, instance->rx.frame_end_gap_us);
    if(message_decoded) {
        instance->signal.message = *message_decoded;
        instance->signal.timings_cnt = 0;
        instance->signal.decoded = true;
        if(instance->rx.received_signal_callback)
            instance->rx.received_signal_callback(
                instance->rx.received_signal_context, &instance->signal);
    }
}

static void infrared_worker_process_timeout(InfraredWorker* instance) {
    if(instance->signal.timings_cnt < 2) return;

    const InfraredMessage* message_decoded =
        infrared_check_decoder_ready(instance->infrared_decoder);
    if(message_decoded) {
        instance->signal.message = *message_decoded;
        instance->signal.timings_cnt = 0;
        instance->signal.decoded = true;
    } else {
        instance->signal.decoded = false;

        if((instance->signal.timings_cnt % 2) == 0) {
            --instance->signal.timings_cnt;
        }

        uint64_t total_duration = 0;
        for(size_t i = 0; i < instance->signal.timings_cnt; i++) {
            total_duration += instance->signal.raw.timings[i];
        }
        if(instance->signal.timings_cnt < INFRARED_WORKER_MIN_RAW_TIMINGS ||
           total_duration < INFRARED_WORKER_MIN_RAW_DURATION_US) {
            infrared_reset_decoder(instance->infrared_decoder);
            return;
        }
    }
    if(instance->rx.received_signal_callback)
        instance->rx.received_signal_callback(
            instance->rx.received_signal_context, &instance->signal);
    if(!message_decoded) {
        infrared_reset_decoder(instance->infrared_decoder);
    }
}

static void
    infrared_worker_process_timings(InfraredWorker* instance, uint32_t duration, bool level) {
    const InfraredMessage* message_decoded =
        instance->decode_enable ? infrared_decode(instance->infrared_decoder, level, duration) :
                                  NULL;
    if(message_decoded) {
        instance->signal.message = *message_decoded;
        instance->signal.timings_cnt = 0;
        instance->signal.decoded = true;
        if(instance->rx.received_signal_callback)
            instance->rx.received_signal_callback(
                instance->rx.received_signal_context, &instance->signal);
    } else {
        /* The HAL delivers the synthetic frame gap through the separate
         * frame-end callback, so it must never enter raw timings. */
        /* Skip first timing if it starts from Space */
        if((instance->signal.timings_cnt == 0) && !level) {
            return;
        }

        if(instance->signal.timings_cnt < MAX_TIMINGS_AMOUNT) {
            instance->signal.raw.timings[instance->signal.timings_cnt] = duration;
            ++instance->signal.timings_cnt;
        } else {
            uint32_t flags_set = furi_thread_flags_set(
                furi_thread_get_id(instance->thread), INFRARED_WORKER_OVERRUN);
            furi_check(flags_set & INFRARED_WORKER_OVERRUN);
            instance->rx.overrun = true;
        }
    }
}

static int32_t infrared_worker_rx_thread(void* thread_context) {
    InfraredWorker* instance = thread_context;
    uint32_t events = 0;
    LevelDuration level_duration;
    uint32_t last_blink_time = 0;

    while(1) {
        events = furi_thread_flags_wait(INFRARED_WORKER_ALL_RX_EVENTS, 0, FuriWaitForever);
        furi_check(events & INFRARED_WORKER_ALL_RX_EVENTS); /* at least one caught */

        if(events & INFRARED_WORKER_RX_ERROR) {
            /* The HAL could not re-arm the receiver. Stopping it (and leaving the
             * state machine) is rx_stop()'s job on the owner thread: doing it here
             * raced with a concurrent rx_stop() and left this thread running
             * unjoined, so the next rx_start()/free() hit a live thread. The owner
             * is told through the error callback so it can call rx_stop(). */
            FURI_LOG_E("IR", "RX stopped receiving after a hardware error");
            furi_check(furi_stream_buffer_reset(instance->rx_stream) == FuriStatusOk);
            infrared_reset_decoder(instance->infrared_decoder);
            instance->signal.timings_cnt = 0;
            instance->rx.overrun = false;
            /* An EXIT in the same batch means the owner is already stopping. */
            if(events & INFRARED_WORKER_EXIT) break;
            InfraredWorkerRxErrorCallback error_callback = instance->rx_error_callback;
            if(error_callback) error_callback(instance->rx_error_context);
            continue;
        }

        if(events & INFRARED_WORKER_RX_RECEIVED) {
            if(!instance->rx.overrun && instance->blink_enable &&
               ((furi_get_tick() - last_blink_time) > 80)) {
                last_blink_time = furi_get_tick();
                notification_message(instance->notification, &sequence_blink_blue_10);
            }
            if(instance->signal.timings_cnt == 0)
                notification_message(instance->notification, &sequence_display_backlight_on);
            while(sizeof(LevelDuration) ==
                  furi_stream_buffer_receive(
                      instance->rx_stream, &level_duration, sizeof(LevelDuration), 0)) {
                if(!instance->rx.overrun) {
                    bool level = level_duration_get_level(level_duration);
                    uint32_t duration = level_duration_get_duration(level_duration);
                    infrared_worker_process_timings(instance, duration, level);
                }
            }
        }
        if(events & INFRARED_WORKER_OVERRUN) {
            printf("#");
            furi_check(furi_stream_buffer_reset(instance->rx_stream) == FuriStatusOk);
            infrared_reset_decoder(instance->infrared_decoder);
            instance->signal.timings_cnt = 0;
            instance->rx.overrun = false;
            if(instance->blink_enable)
                notification_message(instance->notification, &sequence_set_red_255);
        }
        if(events & INFRARED_WORKER_RX_FRAME_END) {
            infrared_worker_process_frame_end(instance);
        }
        if(events & INFRARED_WORKER_RX_TIMEOUT_RECEIVED) {
            if(instance->rx.overrun) {
                printf("\nOVERRUN, max samples: %d\n", MAX_TIMINGS_AMOUNT);
                instance->rx.overrun = false;
                if(instance->blink_enable)
                    notification_message(instance->notification, &sequence_reset_red);
            } else {
                infrared_worker_process_timeout(instance);
            }
            instance->signal.timings_cnt = 0;
        }
        if(events & INFRARED_WORKER_EXIT) break;
    }

    return 0;
}

void infrared_worker_rx_set_received_signal_callback(
    InfraredWorker* instance,
    InfraredWorkerReceivedSignalCallback callback,
    void* context) {
    furi_check(instance);

    instance->rx.received_signal_callback = callback;
    instance->rx.received_signal_context = context;
}

void infrared_worker_rx_set_error_callback(
    InfraredWorker* instance,
    InfraredWorkerRxErrorCallback callback,
    void* context) {
    furi_check(instance);

    instance->rx_error_context = context;
    instance->rx_error_callback = callback;
}

InfraredWorker* infrared_worker_alloc(void) {
    InfraredWorker* instance =
        heap_caps_malloc(sizeof(InfraredWorker), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!instance) return NULL;

    instance->thread = furi_thread_alloc_ex("InfraredWorker", 4096, NULL, instance);
    if(!instance->thread) goto cleanup_instance;

    instance->rx_stream = NULL;
    instance->rx_error_callback = NULL;
    instance->rx_error_context = NULL;
    instance->tx_stream = furi_stream_buffer_alloc(
        sizeof(InfraredWorkerTiming) * (MAX_TIMINGS_AMOUNT + 1), sizeof(InfraredWorkerTiming));
    if(!instance->tx_stream) goto cleanup_thread;
    instance->infrared_decoder = infrared_alloc_decoder();
    if(!instance->infrared_decoder) goto cleanup_stream;
    instance->infrared_encoder = infrared_alloc_encoder();
    if(!instance->infrared_encoder) goto cleanup_decoder;
    instance->blink_enable = false;
    instance->decode_enable = true;
    instance->notification = furi_record_open(RECORD_NOTIFICATION);
    instance->state = InfraredWorkerStateIdle;

    return instance;

cleanup_decoder:
    infrared_free_decoder(instance->infrared_decoder);
cleanup_stream:
    furi_stream_buffer_free(instance->tx_stream);
cleanup_thread:
    furi_thread_free(instance->thread);
cleanup_instance:
    free(instance);
    return NULL;
}

void infrared_worker_free(InfraredWorker* instance) {
    furi_check(instance);
    furi_check(instance->state == InfraredWorkerStateIdle);

    furi_record_close(RECORD_NOTIFICATION);
    infrared_free_decoder(instance->infrared_decoder);
    infrared_free_encoder(instance->infrared_encoder);
    furi_check(instance->rx_stream == NULL);
    furi_stream_buffer_free(instance->tx_stream);
    furi_thread_free(instance->thread);

    free(instance);
}

bool infrared_worker_rx_start(InfraredWorker* instance) {
    furi_check(instance);
    furi_check(instance->state == InfraredWorkerStateIdle);
    if(!furi_hal_infrared_is_available()) return false;
    if(!furi_hal_infrared_claim_rx()) return false;

    instance->rx_stream = furi_stream_buffer_try_alloc_internal(
        sizeof(LevelDuration) * MAX_TIMINGS_AMOUNT, sizeof(LevelDuration));
    if(!instance->rx_stream) {
        FURI_LOG_E("IR", "No internal RAM for the RX buffer");
        furi_hal_infrared_release_claim();
        return false;
    }

    infrared_reset_decoder(instance->infrared_decoder);
    instance->signal.timings_cnt = 0;
    instance->rx.overrun = false;

    furi_thread_set_callback(instance->thread, infrared_worker_rx_thread);
    furi_thread_start(instance->thread);

    furi_hal_infrared_async_rx_set_capture_isr_callback(infrared_worker_rx_callback, instance);
    furi_hal_infrared_async_rx_set_timeout_isr_callback(
        infrared_worker_rx_timeout_callback, instance);
    furi_hal_infrared_async_rx_set_frame_end_isr_callback(
        infrared_worker_rx_frame_end_callback, instance);
    furi_hal_infrared_async_rx_set_error_callback(infrared_worker_rx_error_callback, instance);
    furi_hal_infrared_async_rx_set_timeout(INFRARED_WORKER_RX_TIMEOUT);
    if(!furi_hal_infrared_async_rx_start()) {
        furi_hal_infrared_async_rx_set_timeout_isr_callback(NULL, NULL);
        furi_hal_infrared_async_rx_set_capture_isr_callback(NULL, NULL);
        furi_hal_infrared_async_rx_set_frame_end_isr_callback(NULL, NULL);
        furi_hal_infrared_async_rx_set_error_callback(NULL, NULL);
        furi_hal_infrared_async_rx_set_timeout(0);
        furi_hal_infrared_release_claim();
        furi_thread_flags_set(furi_thread_get_id(instance->thread), INFRARED_WORKER_EXIT);
        furi_thread_join(instance->thread);
        furi_stream_buffer_free(instance->rx_stream);
        instance->rx_stream = NULL;
        return false;
    }

    instance->state = InfraredWorkerStateRunRx;
    infrared_worker_light_sleep_inhibit_begin(instance);
    return true;
}

void infrared_worker_rx_stop(InfraredWorker* instance) {
    furi_check(instance);
    if(instance->state == InfraredWorkerStateIdle) return;
    if(instance->state != InfraredWorkerStateRunRx) {
        FURI_LOG_W("IR", "RX stop ignored: worker is not receiving");
        return;
    }

    furi_hal_infrared_async_rx_stop();

    furi_thread_flags_set(furi_thread_get_id(instance->thread), INFRARED_WORKER_EXIT);
    furi_thread_join(instance->thread);

    furi_stream_buffer_free(instance->rx_stream);
    instance->rx_stream = NULL;
    infrared_reset_decoder(instance->infrared_decoder);
    instance->signal.timings_cnt = 0;

    instance->state = InfraredWorkerStateIdle;
    infrared_worker_light_sleep_inhibit_end(instance);
}

bool infrared_worker_signal_is_decoded(const InfraredWorkerSignal* signal) {
    furi_check(signal);

    return signal->decoded;
}

void infrared_worker_get_raw_signal(
    const InfraredWorkerSignal* signal,
    const uint32_t** timings,
    size_t* timings_cnt) {
    furi_check(signal);
    furi_check(timings);
    furi_check(timings_cnt);

    *timings = signal->raw.timings;
    *timings_cnt = signal->timings_cnt;
}

const InfraredMessage* infrared_worker_get_decoded_signal(const InfraredWorkerSignal* signal) {
    furi_check(signal);

    return &signal->message;
}

void infrared_worker_rx_enable_blink_on_receiving(InfraredWorker* instance, bool enable) {
    furi_check(instance);

    instance->blink_enable = enable;
}

void infrared_worker_rx_enable_signal_decoding(InfraredWorker* instance, bool enable) {
    furi_check(instance);

    instance->decode_enable = enable;
}

bool infrared_worker_tx_start(InfraredWorker* instance) {
    furi_check(instance);
    furi_check(instance->state == InfraredWorkerStateIdle);
    furi_check(instance->tx.get_signal_callback);
    if(!furi_hal_infrared_is_available()) return false;
    /* Fail here, where the caller can tell the user, not later in the TX thread. */
    if(!furi_hal_infrared_is_tx_output_free()) return false;
    if(!furi_hal_infrared_claim_tx()) return false;

    // size have to be greater than api hal infrared async tx buffer size
    furi_stream_set_trigger_level(instance->tx_stream, sizeof(InfraredWorkerTiming));

    furi_thread_set_callback(instance->thread, infrared_worker_tx_thread);

    instance->tx.steady_signal_sent = false;
    instance->tx.need_reinitialization = false;
    instance->tx.underrun = false;
    instance->tx.overflow = false;
    instance->tx.error = false;
    furi_hal_infrared_async_tx_set_data_isr_callback(
        infrared_worker_furi_hal_data_isr_callback, instance);
    furi_hal_infrared_async_tx_set_signal_sent_isr_callback(
        infrared_worker_furi_hal_message_sent_isr_callback, instance);
    furi_hal_infrared_async_tx_set_overflow_isr_callback(
        infrared_worker_furi_hal_overflow_isr_callback, instance);
    furi_hal_infrared_async_tx_set_error_callback(
        infrared_worker_furi_hal_error_callback, instance);

    instance->state = InfraredWorkerStateStartTx;
    infrared_worker_light_sleep_inhibit_begin(instance);
    furi_thread_start(instance->thread);
    return true;
}

static void infrared_worker_furi_hal_message_sent_isr_callback(void* context) {
    InfraredWorker* instance = context;
    /* ESP32 port: same task-context race as data_callback — see comment there. */
    furi_thread_flags_set(
        furi_thread_get_id(instance->thread), INFRARED_WORKER_TX_MESSAGE_SENT);
}

static void infrared_worker_furi_hal_overflow_isr_callback(void* context) {
    InfraredWorker* instance = context;
    instance->tx.overflow = true;
    furi_thread_flags_set(
        furi_thread_get_id(instance->thread), INFRARED_WORKER_TX_OVERFLOW);
}

static void infrared_worker_furi_hal_error_callback(void* context) {
    InfraredWorker* instance = context;
    instance->tx.error = true;
    furi_thread_flags_set(
        furi_thread_get_id(instance->thread), INFRARED_WORKER_TX_ERROR);
}

static FuriHalInfraredTxGetDataState
    infrared_worker_furi_hal_data_isr_callback(void* context, uint32_t* duration, bool* level) {
    furi_assert(context);
    furi_assert(duration);
    furi_assert(level);

    InfraredWorker* instance = context;
    InfraredWorkerTiming timing;
    FuriHalInfraredTxGetDataState state;

    if(sizeof(InfraredWorkerTiming) ==
       furi_stream_buffer_receive(instance->tx_stream, &timing, sizeof(InfraredWorkerTiming), 0)) {
        *level = timing.level;
        *duration = timing.duration;
        state = timing.state;
    } else {
        FURI_LOG_W("IR", "TX underrun - aborting transmission");
        instance->tx.underrun = true;
        *level = 0;
        *duration = 100;
        state = FuriHalInfraredTxGetDataStateLastDone;
        furi_thread_flags_set(
            furi_thread_get_id(instance->thread), INFRARED_WORKER_TX_UNDERRUN);
        return state;
    }

    /* ESP32 port: data_callback runs from ir_tx_task (priority 15), not ISR.
     * The worker thread (priority 16) preempts between notify-set and read-back,
     * consuming the flag before the query — the upstream furi_check on the
     * read-back is unsafe in task context. The set itself can't fail with a
     * valid thread handle, so just fire and forget. */
    furi_thread_flags_set(
        furi_thread_get_id(instance->thread), INFRARED_WORKER_TX_FILL_BUFFER);

    return state;
}

static bool infrared_get_new_signal(InfraredWorker* instance) {
    bool new_signal_obtained = false;

    InfraredWorkerGetSignalResponse response =
        instance->tx.get_signal_callback(instance->tx.get_signal_context, instance);
    if(response == InfraredWorkerGetSignalResponseNew) {
        uint32_t new_tx_frequency = 0;
        float new_tx_duty_cycle = 0;
        if(instance->signal.decoded) {
            new_tx_frequency = infrared_get_protocol_frequency(instance->signal.message.protocol);
            new_tx_duty_cycle =
                infrared_get_protocol_duty_cycle(instance->signal.message.protocol);
        } else {
            furi_assert(instance->signal.timings_cnt > 1);
            new_tx_frequency = instance->signal.raw.frequency;
            new_tx_duty_cycle = instance->signal.raw.duty_cycle;
        }

        instance->tx.tx_raw_cnt = 0;
        instance->tx.need_reinitialization =
            (new_tx_frequency != instance->tx.frequency) ||
            !float_is_equal(new_tx_duty_cycle, instance->tx.duty_cycle);
        instance->tx.frequency = new_tx_frequency;
        instance->tx.duty_cycle = new_tx_duty_cycle;
        if(instance->signal.decoded) {
            infrared_reset_encoder(instance->infrared_encoder, &instance->signal.message);
        }
        new_signal_obtained = true;
    } else if(response == InfraredWorkerGetSignalResponseSame) {
        new_signal_obtained = true;
        /* no need to reinit */
    } else if(response == InfraredWorkerGetSignalResponseStop) {
        new_signal_obtained = false;
    } else {
        furi_crash();
    }

    return new_signal_obtained;
}

static bool infrared_worker_tx_fill_buffer(InfraredWorker* instance) {
    bool new_data_available = true;
    InfraredWorkerTiming timing;
    InfraredStatus status = InfraredStatusError;

    while(!furi_stream_buffer_is_full(instance->tx_stream) && !instance->tx.need_reinitialization &&
          new_data_available) {
        if(instance->signal.decoded) {
            status = infrared_encode(instance->infrared_encoder, &timing.duration, &timing.level);
        } else {
            timing.duration = instance->signal.raw.timings[instance->tx.tx_raw_cnt];

            timing.level = (instance->tx.tx_raw_cnt % 2);
            ++instance->tx.tx_raw_cnt;
            if(instance->tx.tx_raw_cnt >= instance->signal.timings_cnt) {
                instance->tx.tx_raw_cnt = 0;
                status = InfraredStatusDone;
            } else {
                status = InfraredStatusOk;
            }
        }

        if(status == InfraredStatusError) {
            new_data_available = false;
            furi_crash();
        } else if(status == InfraredStatusOk) {
            timing.state = FuriHalInfraredTxGetDataStateOk;
        } else if(status == InfraredStatusDone) {
            timing.state = FuriHalInfraredTxGetDataStateDone;

            new_data_available = infrared_get_new_signal(instance);
            if(instance->tx.need_reinitialization || !new_data_available) {
                timing.state = FuriHalInfraredTxGetDataStateLastDone;
            }
        } else {
            furi_crash();
        }
        uint32_t written_size =
            furi_stream_buffer_send(instance->tx_stream, &timing, sizeof(InfraredWorkerTiming), 0);
        furi_assert(sizeof(InfraredWorkerTiming) == written_size);
        (void)written_size;
    }

    return new_data_available;
}

static int32_t infrared_worker_tx_thread(void* thread_context) {
    InfraredWorker* instance = thread_context;
    furi_assert(instance->state == InfraredWorkerStateStartTx);
    furi_assert(thread_context);

    size_t repeats_left =
        instance->signal.decoded ?
            infrared_get_protocol_min_repeat_count(instance->signal.message.protocol) :
            1;
    uint32_t events = 0;

    bool exit_pending = false;

    bool running = infrared_get_new_signal(instance);
    if(!running) {
        furi_hal_infrared_async_tx_set_data_isr_callback(NULL, NULL);
        furi_hal_infrared_async_tx_set_signal_sent_isr_callback(NULL, NULL);
        furi_hal_infrared_async_tx_set_overflow_isr_callback(NULL, NULL);
        furi_hal_infrared_async_tx_set_error_callback(NULL, NULL);
        furi_hal_infrared_release_claim();
        /* Not Idle: tx_stop() must still join this thread before it can restart. */
        instance->state = InfraredWorkerStateStopTx;
        return 0;
    }

    while(running) {
        switch(instance->state) {
        case InfraredWorkerStateStartTx:
            if(repeats_left > 0) {
                --repeats_left;
            } /* The first message does not result in TX_MESSAGE_SENT event. */
            instance->tx.need_reinitialization = false;
            const bool new_data_available = infrared_worker_tx_fill_buffer(instance);
            if(!furi_hal_infrared_async_tx_start(
                   instance->tx.frequency, instance->tx.duty_cycle)) {
                instance->tx.error = true;
                furi_hal_infrared_async_tx_set_data_isr_callback(NULL, NULL);
                furi_hal_infrared_async_tx_set_signal_sent_isr_callback(NULL, NULL);
                furi_hal_infrared_async_tx_set_overflow_isr_callback(NULL, NULL);
                furi_hal_infrared_async_tx_set_error_callback(NULL, NULL);
                furi_hal_infrared_release_claim();
                instance->state = InfraredWorkerStateStopTx;
                break;
            }

            if(!new_data_available) {
                instance->state = InfraredWorkerStateStopTx;
            } else if(instance->tx.need_reinitialization) {
                instance->state = InfraredWorkerStateWaitTxEnd;
            } else {
                instance->state = InfraredWorkerStateRunTx;
            }

            break;
        case InfraredWorkerStateStopTx:
            if(furi_hal_infrared_is_busy()) {
                furi_hal_infrared_async_tx_stop();
            }
            running = false;
            break;
        case InfraredWorkerStateWaitTxEnd:
            furi_hal_infrared_async_tx_wait_termination();
            instance->state = InfraredWorkerStateStartTx;

            events = furi_thread_flags_get();
            if(events & INFRARED_WORKER_EXIT) {
                running = false;
                break;
            }

            break;
        case InfraredWorkerStateRunTx:
            events = furi_thread_flags_wait(
                INFRARED_WORKER_ALL_TX_EVENTS, FuriFlagWaitAny, FuriWaitForever);
            furi_check(events & INFRARED_WORKER_ALL_TX_EVENTS); /* at least one caught */

            if(events & INFRARED_WORKER_EXIT) {
                exit_pending = true;
            }

            if((events & INFRARED_WORKER_TX_FILL_BUFFER) &&
               !(events & INFRARED_WORKER_TX_UNDERRUN) &&
                    !(events & INFRARED_WORKER_TX_OVERFLOW) &&
                    !(events & INFRARED_WORKER_TX_ERROR)) {
                infrared_worker_tx_fill_buffer(instance);

                if(instance->tx.need_reinitialization) {
                    instance->state = InfraredWorkerStateWaitTxEnd;
                }
            }

            if(events & INFRARED_WORKER_TX_UNDERRUN) {
                instance->state = InfraredWorkerStateStopTx;
            }

            if(events & INFRARED_WORKER_TX_OVERFLOW) {
                instance->state = InfraredWorkerStateStopTx;
            }

            if(events & INFRARED_WORKER_TX_ERROR) {
                instance->state = InfraredWorkerStateStopTx;
            }

            if((events & INFRARED_WORKER_TX_MESSAGE_SENT) &&
                    !instance->tx.underrun && !instance->tx.overflow && !instance->tx.error) {
                if(repeats_left > 0) {
                    --repeats_left;
                }

                if(instance->tx.message_sent_callback) {
                    instance->tx.message_sent_callback(instance->tx.message_sent_context);
                }
            }

            if(exit_pending && repeats_left == 0) {
                instance->state = InfraredWorkerStateStopTx;
            }

            break;
        default:
            furi_crash();
            break;
        }
    }

    return 0;
}

void infrared_worker_tx_set_get_signal_callback(
    InfraredWorker* instance,
    InfraredWorkerGetSignalCallback callback,
    void* context) {
    furi_check(instance);

    instance->tx.get_signal_callback = callback;
    instance->tx.get_signal_context = context;
}

void infrared_worker_tx_set_signal_sent_callback(
    InfraredWorker* instance,
    InfraredWorkerMessageSentCallback callback,
    void* context) {
    furi_check(instance);

    instance->tx.message_sent_callback = callback;
    instance->tx.message_sent_context = context;
}

void infrared_worker_tx_stop(InfraredWorker* instance) {
    furi_check(instance);
    if(instance->state == InfraredWorkerStateIdle ||
       instance->state == InfraredWorkerStateRunRx) {
        return;
    }

    furi_thread_flags_set(furi_thread_get_id(instance->thread), INFRARED_WORKER_EXIT);
    furi_thread_join(instance->thread);

    instance->signal.timings_cnt = 0;
    furi_check(furi_stream_buffer_reset(instance->tx_stream) == FuriStatusOk);

    instance->state = InfraredWorkerStateIdle;
    infrared_worker_light_sleep_inhibit_end(instance);
}

void infrared_worker_set_decoded_signal(InfraredWorker* instance, const InfraredMessage* message) {
    furi_check(instance);
    furi_check(message);

    instance->signal.decoded = true;
    instance->signal.message = *message;
}

void infrared_worker_set_raw_signal(
    InfraredWorker* instance,
    const uint32_t* timings,
    size_t timings_cnt,
    uint32_t frequency,
    float duty_cycle) {
    furi_check(instance);
    furi_check(timings);
    furi_check(timings_cnt > 0);
    furi_check((frequency <= INFRARED_MAX_FREQUENCY) && (frequency >= INFRARED_MIN_FREQUENCY));
    furi_check((duty_cycle <= 1.0f) && (duty_cycle > 0.0f));

    size_t max_copy_num = COUNT_OF(instance->signal.raw.timings) - 1;
    furi_check(timings_cnt <= max_copy_num);

    instance->signal.raw.frequency = frequency;
    instance->signal.raw.duty_cycle = duty_cycle;
    instance->signal.raw.timings[0] = INFRARED_RAW_TX_TIMING_DELAY_US;
    memcpy(&instance->signal.raw.timings[1], timings, timings_cnt * sizeof(uint32_t));
    instance->signal.decoded = false;
    instance->signal.timings_cnt = timings_cnt + 1;
}

InfraredWorkerGetSignalResponse
    infrared_worker_tx_get_signal_steady_callback(void* context, InfraredWorker* instance) {
    UNUSED(context);
    furi_check(instance);

    InfraredWorkerGetSignalResponse response = instance->tx.steady_signal_sent ?
                                                   InfraredWorkerGetSignalResponseSame :
                                                   InfraredWorkerGetSignalResponseNew;
    instance->tx.steady_signal_sent = true;
    return response;
}
