/**
 * @file furi_hal_infrared.c
 * ESP32 Infrared HAL implementation using RMT peripheral.
 *
 * RX: RMT receiver captures edges, a GPTimer provides silence timeout.
 * TX: RMT transmitter with carrier modulation, fed via encode callback.
 */

#include <furi_hal_infrared.h>
#include <furi_hal_gpio.h>
#include <furi.h>

#include <limits.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <driver/rmt_tx.h>
#include <driver/rmt_rx.h>
#include <driver/rmt_encoder.h>
#include <driver/gptimer.h>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>

#include BOARD_INCLUDE

/* ---- Configuration ---- */

#if BOARD_HAS_IR
#define IR_TX_GPIO  BOARD_PIN_IR_TX
#define IR_RX_GPIO  BOARD_PIN_IR_RX
#else
#define IR_TX_GPIO  GPIO_NUM_NC
#define IR_RX_GPIO  GPIO_NUM_NC
#endif

#if BOARD_HAS_IR
#define IR_TX_INT_SUPPORTED 1
#else
#define IR_TX_INT_SUPPORTED 0
#endif

/* External IR output on the expansion header, shared with GPIO manual control.
 * It is reserved and driven only while a transmission runs. */
#if BOARD_HAS_IR && defined(BOARD_PIN_IR_TX_EXT)
#define IR_TX_EXT_SUPPORTED 1
static const char IR_TX_EXT_OWNER[] = "Infrared";
static const GpioPin ir_tx_ext_pin = {.port = NULL, .pin = BOARD_PIN_IR_TX_EXT};
#else
#define IR_TX_EXT_SUPPORTED 0
#endif

#define IR_RMT_RX_MEM_BLOCK_SYMBOLS 128
#define IR_RMT_RX_RESOLUTION_HZ     1000000 /* 1 MHz = 1 us per tick */
#define IR_RMT_TX_RESOLUTION_HZ     1000000 /* 1 MHz = 1 us per tick */
#define IR_RMT_RX_MAX_SYMBOLS       1024
#define IR_TX_MAX_SYMBOLS           700

/* ---- State ---- */

typedef enum {
    InfraredStateIdle,
    InfraredStateStartingRx,
    InfraredStateAsyncRx,
    InfraredStateStartingTx,
    InfraredStateAsyncTx,
    InfraredStateAsyncTxStopReq,
    InfraredStateAsyncTxStopped,
    InfraredStateMAX,
} InfraredState;

#define IR_RX_REARM_TASK_STACK_SIZE 2048
#define IR_RX_REARM_TASK_PRIORITY 17
#define IR_RX_REARM_NOTIFY_RECEIVE_DONE (1UL << 0)
#define IR_RX_REARM_NOTIFY_EXIT (1UL << 1)
#define IR_RX_REARM_NOTIFY_TIMEOUT_UPDATE (1UL << 2)

/* ISR/HAL state must live in internal DRAM: Furi thread stacks are in PSRAM,
 * and CONFIG_GPTIMER_ISR_IRAM_SAFE requires alarm/callback structs in RAM
 * that remains mapped when the cache is disabled. */
static DRAM_ATTR portMUX_TYPE ir_rx_timer_lock = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR volatile uint32_t ir_rx_timer_users;
static DRAM_ATTR gptimer_config_t ir_rx_timer_config;
static DRAM_ATTR gptimer_alarm_config_t ir_rx_alarm_config;
static DRAM_ATTR gptimer_event_callbacks_t ir_rx_timer_cbs;
/* A timeout timer whose teardown failed. Its handle is already detached from
 * ir_rx, so it is parked here and reclaimed before the next timer is created.
 * Owner-thread only (set_timeout / rx_stop / start failure are serialized by
 * the ir_state claim), never touched from an ISR. At most one can exist: a new
 * timer is only created while this slot is empty. */
static gptimer_handle_t ir_rx_timer_orphan;

/* RX state */
static DRAM_ATTR struct {
    rmt_channel_handle_t channel;
    rmt_receive_config_t rx_config;
    rmt_symbol_word_t* symbols;
    size_t symbols_bytes;
    FuriHalInfraredRxCaptureCallback capture_callback;
    void* capture_context;
    FuriHalInfraredRxTimeoutCallback timeout_callback;
    void* timeout_context;
    FuriHalInfraredRxFrameEndCallback frame_end_callback;
    void* frame_end_context;
    FuriHalInfraredRxErrorCallback error_callback;
    void* error_context;
    gptimer_handle_t timeout_timer; /* protected by ir_rx_timer_lock */
    bool timeout_timer_running; /* Actual driver state; protected by ir_rx_timer_lock. */
    bool timeout_timer_should_run; /* Requested state; protected by ir_rx_timer_lock. */
    bool timeout_timer_restart_pending; /* Protected by ir_rx_timer_lock. */
    bool timeout_alarm_armed; /* Protected by ir_rx_timer_lock. */
    uint32_t timeout_generation; /* Protected by ir_rx_timer_lock. */
    uint32_t timeout_us;
    uint32_t frame_end_gap_us;
    TaskHandle_t rearm_task_handle;
    SemaphoreHandle_t rearm_done_semaphore;
} ir_rx;

/* TX state */
static struct {
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t copy_encoder;
    FuriHalInfraredTxGetDataISRCallback data_callback;
    void* data_context;
    FuriHalInfraredTxSignalSentISRCallback signal_sent_callback;
    void* signal_sent_context;
    FuriHalInfraredTxOverflowISRCallback overflow_callback;
    void* overflow_context;
    FuriHalInfraredTxErrorCallback error_callback;
    void* error_context;
    SemaphoreHandle_t done_semaphore;
    bool channel_disabled;
    bool ext_pin_reserved;
    uint32_t carrier_freq;
    float duty_cycle;
} ir_tx;

static volatile FuriHalInfraredTxPin ir_tx_output = FuriHalInfraredTxPinInternal;

static DRAM_ATTR volatile InfraredState ir_state = InfraredStateIdle;
static DRAM_ATTR portMUX_TYPE ir_state_lock = portMUX_INITIALIZER_UNLOCKED;

static void ir_rx_free_symbols(void) {
    if(ir_rx.symbols) {
        heap_caps_free(ir_rx.symbols);
        ir_rx.symbols = NULL;
        ir_rx.symbols_bytes = 0;
    }
}

/* Disable and delete the RMT RX channel. Returns true when no channel is left.
 * On false the channel (and so the symbol buffer the RMT ISR copies into) is
 * still alive: the caller must not free the buffer. ir_rx.channel is kept so a
 * later start can retry the teardown. */
static bool ir_rx_destroy_channel(bool channel_enabled) {
    if(!ir_rx.channel) return true;

    esp_err_t err;
    if(channel_enabled) {
        err = rmt_disable(ir_rx.channel);
        if(err != ESP_OK) {
            FURI_LOG_W("IR", "RX channel disable failed: %s", esp_err_to_name(err));
        }
    }
    err = rmt_del_channel(ir_rx.channel);
    if(err != ESP_OK) {
        FURI_LOG_E("IR", "RX channel delete failed: %s", esp_err_to_name(err));
        return false;
    }
    ir_rx.channel = NULL;
    return true;
}

static bool ir_rx_alloc_symbols(void) {
    if(ir_rx.symbols) return true;

    const size_t bytes = IR_RMT_RX_MAX_SYMBOLS * sizeof(rmt_symbol_word_t);
    rmt_symbol_word_t* symbols = heap_caps_malloc(
        bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if(!symbols) {
        symbols = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if(!symbols) {
        FURI_LOG_E("IR", "RX symbol buffer allocation failed");
        return false;
    }
    ir_rx.symbols = symbols;
    ir_rx.symbols_bytes = bytes;
    return true;
}

static void ir_tx_pin_set_idle(void) {
#if BOARD_HAS_IR
    gpio_reset_pin(IR_TX_GPIO);
    gpio_set_direction(IR_TX_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(IR_TX_GPIO, 0);
#endif
}

/* Take the pin the next transmission drives. The internal LED pin is ours alone;
 * the external header pin must be free and not driven by anyone else. */
static bool ir_tx_acquire_output(FuriHalInfraredTxPin output, gpio_num_t* tx_gpio) {
    *tx_gpio = (gpio_num_t)IR_TX_GPIO;
#if IR_TX_EXT_SUPPORTED
    if(output == FuriHalInfraredTxPinExtPA7) {
        if(!furi_hal_gpio_reserve(&ir_tx_ext_pin, IR_TX_EXT_OWNER)) {
            FURI_LOG_E(
                "IR",
                "External TX pin GPIO%u is in use by %s",
                (unsigned)ir_tx_ext_pin.pin,
                furi_hal_gpio_get_reservation(&ir_tx_ext_pin));
            return false;
        }
        if(furi_hal_gpio_is_output(&ir_tx_ext_pin)) {
            furi_hal_gpio_unreserve(&ir_tx_ext_pin, IR_TX_EXT_OWNER);
            FURI_LOG_E(
                "IR",
                "External TX pin GPIO%u is driven by another user",
                (unsigned)ir_tx_ext_pin.pin);
            return false;
        }
        ir_tx.ext_pin_reserved = true;
        *tx_gpio = (gpio_num_t)ir_tx_ext_pin.pin;
    }
#else
    UNUSED(output);
#endif
    return true;
}

/* Back to idle: internal LED off, external header pin released undriven but
 * pulled down so neither an attached IR driver nor another user sees it float. */
static void ir_tx_release_output(void) {
    if(!ir_tx.ext_pin_reserved) {
        ir_tx_pin_set_idle();
    }
#if IR_TX_EXT_SUPPORTED
    if(ir_tx.ext_pin_reserved) {
        const gpio_num_t pin = (gpio_num_t)ir_tx_ext_pin.pin;
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
        ir_tx.ext_pin_reserved = false;
        furi_hal_gpio_unreserve(&ir_tx_ext_pin, IR_TX_EXT_OWNER);
    }
#endif
}

void furi_hal_infrared_init(void) {
    ir_tx_pin_set_idle();
}

static bool ir_state_try_claim(InfraredState starting_state) {
    bool claimed = false;

    portENTER_CRITICAL(&ir_state_lock);
    if(ir_state == InfraredStateIdle) {
        ir_state = starting_state;
        claimed = true;
    }
    portEXIT_CRITICAL(&ir_state_lock);

    return claimed;
}

static void ir_state_set(InfraredState state) {
    portENTER_CRITICAL(&ir_state_lock);
    ir_state = state;
    portEXIT_CRITICAL(&ir_state_lock);
}

static void ir_state_advance(InfraredState from, InfraredState to) {
    bool valid = false;

    portENTER_CRITICAL(&ir_state_lock);
    if(ir_state == from) {
        ir_state = to;
        valid = true;
    }
    portEXIT_CRITICAL(&ir_state_lock);

    furi_check(valid);
}

static void ir_state_release_claim_internal(void) {
    bool valid = false;

    portENTER_CRITICAL(&ir_state_lock);
    if(ir_state == InfraredStateStartingRx || ir_state == InfraredStateStartingTx) {
        ir_state = InfraredStateIdle;
        valid = true;
    }
    portEXIT_CRITICAL(&ir_state_lock);

    furi_check(valid);
}

static void ir_state_request_tx_stop(void) {
    portENTER_CRITICAL(&ir_state_lock);
    if(ir_state == InfraredStateAsyncTx) {
        ir_state = InfraredStateAsyncTxStopReq;
    }
    portEXIT_CRITICAL(&ir_state_lock);
}

bool furi_hal_infrared_claim_rx(void) {
    return ir_state_try_claim(InfraredStateStartingRx);
}

bool furi_hal_infrared_claim_tx(void) {
    return ir_state_try_claim(InfraredStateStartingTx);
}

void furi_hal_infrared_release_claim(void) {
    ir_state_release_claim_internal();
}

/* ---- RX Implementation ---- */

static bool IRAM_ATTR ir_rx_request_timeout_restart(void) {
    bool requested = false;
    portENTER_CRITICAL_ISR(&ir_rx_timer_lock);
    if(ir_rx.timeout_timer && ir_state == InfraredStateAsyncRx) {
        ir_rx.timeout_timer_should_run = true;
        ir_rx.timeout_timer_restart_pending = true;
        ir_rx.timeout_alarm_armed = false;
        ir_rx.timeout_generation++;
        requested = true;
    }
    portEXIT_CRITICAL_ISR(&ir_rx_timer_lock);
    return requested;
}

static void ir_rx_apply_timeout_update(void) {
    gptimer_handle_t timer;
    bool timer_running;
    bool timer_should_run;
    bool restart_pending;
    uint32_t timeout_generation;

    portENTER_CRITICAL(&ir_rx_timer_lock);
    timer = ir_rx.timeout_timer;
    timer_running = ir_rx.timeout_timer_running;
    timer_should_run = ir_rx.timeout_timer_should_run;
    restart_pending = ir_rx.timeout_timer_restart_pending;
    timeout_generation = ir_rx.timeout_generation;
    ir_rx.timeout_timer_restart_pending = false;
    portEXIT_CRITICAL(&ir_rx_timer_lock);

    if(!timer) return;

    esp_err_t err = ESP_OK;
    if(timer_should_run) {
        if(restart_pending) {
            if(timer_running) {
                err = gptimer_stop(timer);
                if(err == ESP_OK) timer_running = false;
            }
            if(err == ESP_OK) err = gptimer_set_raw_count(timer, 0);
        }
        if((err == ESP_OK) && !timer_running) {
            portENTER_CRITICAL(&ir_rx_timer_lock);
            const bool start_requested =
                (ir_rx.timeout_timer == timer) &&
                ir_rx.timeout_timer_should_run &&
                (ir_rx.timeout_generation == timeout_generation);
            if(start_requested) ir_rx.timeout_alarm_armed = true;
            portEXIT_CRITICAL(&ir_rx_timer_lock);

            if(start_requested) {
                err = gptimer_start(timer);
                if(err == ESP_OK) {
                    timer_running = true;
                } else {
                    portENTER_CRITICAL(&ir_rx_timer_lock);
                    if(ir_rx.timeout_timer == timer) {
                        ir_rx.timeout_alarm_armed = false;
                    }
                    portEXIT_CRITICAL(&ir_rx_timer_lock);
                }
            }
        }
    } else if(timer_running) {
        err = gptimer_stop(timer);
        if(err == ESP_OK) timer_running = false;
    }

    portENTER_CRITICAL(&ir_rx_timer_lock);
    if(ir_rx.timeout_timer == timer) {
        ir_rx.timeout_timer_running = timer_running;
        if((err != ESP_OK) && ir_rx.timeout_timer_should_run) {
            ir_rx.timeout_timer_restart_pending = true;
        }
    }
    portEXIT_CRITICAL(&ir_rx_timer_lock);

    if(err != ESP_OK) {
        FURI_LOG_E("IR", "RX timeout timer update failed: %s", esp_err_to_name(err));
    }
}

/* Stop (if it may be running), disable and delete a detached timeout timer.
 * Returns true when the timer is gone. gptimer_del_timer() only succeeds from
 * the init state, so a failed stop/disable normally shows up as a failed delete
 * too; the caller then keeps the handle to retry instead of leaking it. */
static bool ir_rx_timer_destroy(gptimer_handle_t timer, bool maybe_running) {
    esp_err_t err;

    if(maybe_running) {
        err = gptimer_stop(timer);
        if(err != ESP_OK) {
            FURI_LOG_W("IR", "RX timeout timer stop failed: %s", esp_err_to_name(err));
        }
    }
    err = gptimer_disable(timer);
    if(err != ESP_OK) {
        FURI_LOG_W("IR", "RX timeout timer disable failed: %s", esp_err_to_name(err));
    }
    err = gptimer_del_timer(timer);
    if(err != ESP_OK) {
        FURI_LOG_E("IR", "RX timeout timer delete failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* Retry the teardown of a timer parked by a failed delete. Returns true when
 * the slot is free again. */
static bool ir_rx_reclaim_orphan_timer(void) {
    if(!ir_rx_timer_orphan) return true;

    /* Its run state is unknown now, so ask for a stop; a "not running" answer is
     * expected and harmless. */
    if(ir_rx_timer_destroy(ir_rx_timer_orphan, true)) {
        ir_rx_timer_orphan = NULL;
        return true;
    }
    return false;
}

static void ir_rx_delete_timeout_timer(void) {
    gptimer_handle_t timer;

    portENTER_CRITICAL(&ir_rx_timer_lock);
    timer = ir_rx.timeout_timer;
    ir_rx.timeout_timer = NULL;
    const bool timer_running = ir_rx.timeout_timer_running;
    ir_rx.timeout_timer_running = false;
    ir_rx.timeout_timer_should_run = false;
    ir_rx.timeout_timer_restart_pending = false;
    ir_rx.timeout_alarm_armed = false;
    ir_rx.timeout_generation = 0;
    portEXIT_CRITICAL(&ir_rx_timer_lock);

    while(ir_rx_timer_users != 0) {
        taskYIELD();
    }

    if(!ir_rx_reclaim_orphan_timer()) {
        FURI_LOG_W("IR", "RX timeout timer from an earlier session is still stuck");
    }

    if(timer && !ir_rx_timer_destroy(timer, timer_running)) {
        if(ir_rx_timer_orphan == NULL) {
            ir_rx_timer_orphan = timer;
        } else {
            /* Not reachable while set_timeout() keeps its guard (a timer is only
             * created with the slot empty); drop the handle rather than crash. */
            FURI_LOG_E("IR", "RX timeout timer leaked");
        }
    }
}

static bool IRAM_ATTR ir_rx_timeout_isr(gptimer_handle_t timer,
                                         const gptimer_alarm_event_data_t* edata,
                                         void* user_ctx) {
    (void)edata;
    (void)user_ctx;

    FuriHalInfraredRxTimeoutCallback callback = NULL;
    void* callback_context = NULL;
    TaskHandle_t rearm_task = NULL;
    bool timer_matched = false;

    portENTER_CRITICAL_ISR(&ir_rx_timer_lock);
    if(ir_state == InfraredStateAsyncRx && timer == ir_rx.timeout_timer &&
       ir_rx.timeout_alarm_armed) {
        callback = ir_rx.timeout_callback;
        callback_context = ir_rx.timeout_context;
        rearm_task = ir_rx.rearm_task_handle;
        ir_rx.timeout_timer_should_run = false;
        ir_rx.timeout_alarm_armed = false;
        ir_rx_timer_users++;
        timer_matched = true;
    }
    portEXIT_CRITICAL_ISR(&ir_rx_timer_lock);

    BaseType_t higher_priority_task_woken = pdFALSE;
    if(timer_matched) {
        if(rearm_task) {
            xTaskNotifyFromISR(
                rearm_task,
                IR_RX_REARM_NOTIFY_TIMEOUT_UPDATE,
                eSetBits,
                &higher_priority_task_woken);
        }

        portENTER_CRITICAL_ISR(&ir_rx_timer_lock);
        ir_rx_timer_users--;
        portEXIT_CRITICAL_ISR(&ir_rx_timer_lock);

        if(callback) {
            if(callback(callback_context)) higher_priority_task_woken = pdTRUE;
        }
    }

    return higher_priority_task_woken == pdTRUE;
}

static void ir_rx_rearm_task(void* arg) {
    (void)arg;

    for(;;) {
        uint32_t notified = 0;
        xTaskNotifyWait(0, ULONG_MAX, &notified, portMAX_DELAY);

        if(notified & IR_RX_REARM_NOTIFY_EXIT) break;
        if(notified & IR_RX_REARM_NOTIFY_TIMEOUT_UPDATE) {
            ir_rx_apply_timeout_update();
        }
        if((notified & IR_RX_REARM_NOTIFY_RECEIVE_DONE) &&
           ir_state == InfraredStateAsyncRx) {
            esp_err_t err = rmt_receive(
                ir_rx.channel, ir_rx.symbols, ir_rx.symbols_bytes, &ir_rx.rx_config);
            if(err != ESP_OK) {
                FURI_LOG_E("IR", "rmt_receive rearm failed: %d", err);
                FuriHalInfraredRxErrorCallback error_callback = ir_rx.error_callback;
                if(error_callback) {
                    error_callback(ir_rx.error_context);
                }
            }
        }
    }

    xSemaphoreGive(ir_rx.rearm_done_semaphore);
    /* Keep the task handle valid until the RMT channel is disabled and the
     * caller deletes this suspended task. Ignore any late ISR notification. */
    vTaskSuspend(NULL);
}

static void ir_rx_cleanup_start_failure(bool channel_enabled) {
    TaskHandle_t rearm_task = ir_rx.rearm_task_handle;
    if(rearm_task) {
        xTaskNotify(rearm_task, IR_RX_REARM_NOTIFY_EXIT, eSetBits);
        xSemaphoreTake(ir_rx.rearm_done_semaphore, portMAX_DELAY);
    }
    const bool channel_gone = ir_rx_destroy_channel(channel_enabled);
    if(rearm_task) {
        vTaskDelete(rearm_task);
        ir_rx.rearm_task_handle = NULL;
    }
    if(ir_rx.rearm_done_semaphore) {
        vSemaphoreDelete(ir_rx.rearm_done_semaphore);
        ir_rx.rearm_done_semaphore = NULL;
    }
    ir_rx_delete_timeout_timer();
    /* A channel that could not be deleted may still copy into the buffer. Keep
     * it; the next start retries the teardown and reuses the buffer. */
    if(channel_gone) {
        ir_rx_free_symbols();
    }
    ir_state_set(InfraredStateStartingRx);
}

static bool IRAM_ATTR ir_rmt_rx_done_callback(rmt_channel_handle_t channel,
                                                const rmt_rx_done_event_data_t* edata,
                                                void* user_ctx) {
    (void)channel;
    (void)user_ctx;

    FuriHalInfraredRxCaptureCallback capture_callback = ir_rx.capture_callback;
    void* capture_context = ir_rx.capture_context;
    bool last_level_mark = false; /* level of the last edge we delivered */
    bool any_edge = false;
    BaseType_t higher_priority_task_woken = pdFALSE;

    /* Process received symbols and call capture callback for each edge */
    for(size_t i = 0; i < edata->num_symbols; i++) {
        const rmt_symbol_word_t* sym = &edata->received_symbols[i];

        /* Each RMT symbol has two phases: duration0/level0 then duration1/level1.
         * invert_in=true on the RX channel already converts the active-low IR
         * receiver output to logical mark=1/space=0, so pass sym->level through
         * unchanged. Negating here would drop the leader mark via the worker's
         * "skip first space" rule. */
        if(sym->duration0 > 0) {
            last_level_mark = sym->level0;
            any_edge = true;
            if(capture_callback) {
                higher_priority_task_woken |=
                    capture_callback(capture_context, sym->level0, sym->duration0);
            }
        }
        if(sym->duration1 > 0) {
            last_level_mark = sym->level1;
            any_edge = true;
            if(capture_callback) {
                higher_priority_task_woken |=
                    capture_callback(capture_context, sym->level1, sym->duration1);
            }
        }
    }

    /* Notify the worker that an idle-terminated burst is complete. The worker
     * feeds the synthetic gap to the decoder in task context, without adding
     * it to raw timings. */
    if(any_edge && last_level_mark) {
        if(ir_rx.frame_end_callback) {
            higher_priority_task_woken |=
                ir_rx.frame_end_callback(
                    ir_rx.frame_end_context, ir_rx.frame_end_gap_us);
        }
    }

    /* RMT delivers a completed burst as one batch; restart the silence window
     * once after all historical edges have been delivered. */
    uint32_t rearm_notifications = IR_RX_REARM_NOTIFY_RECEIVE_DONE;
    if(any_edge && ir_rx_request_timeout_restart()) {
        rearm_notifications |= IR_RX_REARM_NOTIFY_TIMEOUT_UPDATE;
    }

    if(ir_state == InfraredStateAsyncRx && ir_rx.rearm_task_handle) {
        xTaskNotifyFromISR(
            ir_rx.rearm_task_handle,
            rearm_notifications,
            eSetBits,
            &higher_priority_task_woken);
    }

    return higher_priority_task_woken == pdTRUE;
}

bool furi_hal_infrared_async_rx_start(void) {
    furi_check(ir_state == InfraredStateStartingRx);
#if !BOARD_HAS_IR
    FURI_LOG_E("IR", "Board has no IR support");
    return false;
#endif

    /* A previous session whose channel teardown failed left it (and the buffer
     * it writes into) behind. Retry before creating a second channel. */
    if(!ir_rx_destroy_channel(true)) {
        return false;
    }

    if(!ir_rx_alloc_symbols()) {
        return false;
    }

    /* Configure RMT RX channel */
    rmt_rx_channel_config_t rx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RMT_RX_RESOLUTION_HZ,
        .mem_block_symbols = IR_RMT_RX_MEM_BLOCK_SYMBOLS,
        .gpio_num = IR_RX_GPIO,
        .flags = {
            .invert_in = true, /* IR receiver module output is active-low */
            .with_dma = false,
        },
    };
    esp_err_t err = rmt_new_rx_channel(&rx_chan_config, &ir_rx.channel);
    if(err != ESP_OK) {
        FURI_LOG_E("IR", "RX channel creation failed: %d", err);
        /* The symbol buffer is allocated before the RMT channel. Keep failed
         * starts retryable without retaining an internal-RAM/DMA allocation. */
        ir_rx_free_symbols();
        return false;
    }

    /* Register RX done callback */
    rmt_rx_event_callbacks_t rx_cbs = {
        .on_recv_done = ir_rmt_rx_done_callback,
    };
    err = rmt_rx_register_event_callbacks(ir_rx.channel, &rx_cbs, NULL);
    if(err != ESP_OK) {
        FURI_LOG_E("IR", "RX callback registration failed: %d", err);
        ir_rx_cleanup_start_failure(false);
        return false;
    }

    /* Enable channel */
    err = rmt_enable(ir_rx.channel);
    if(err != ESP_OK) {
        FURI_LOG_E("IR", "RX channel enable failed: %d", err);
        ir_rx_cleanup_start_failure(false);
        return false;
    }

    /* Configure receive parameters */
    ir_rx.rx_config.signal_range_min_ns = 1250;  /* glitch filter: ignore < 1.25us */
    /* RMT idle threshold: silence longer than this ends the frame.
       Max at 1 MHz resolution is ~32.7 ms (RMT_LL_MAX_IDLE_VALUE=32767).
       Use 20 ms default which covers all common IR protocols. */
    uint32_t timeout_us = ir_rx.timeout_us > 0 ? ir_rx.timeout_us : 20000;
    if(timeout_us > 32000) timeout_us = 32000;
    ir_rx.frame_end_gap_us = timeout_us;
    uint32_t max_ns = timeout_us * 1000;
    if(max_ns > 32000 * 1000) max_ns = 32000 * 1000; /* clamp to RMT HW limit */
    ir_rx.rx_config.signal_range_max_ns = max_ns;
    ir_rx.rx_config.flags.en_partial_rx = false;

    if((ir_rx.timeout_us > 0) && (ir_rx.timeout_timer == NULL)) {
        FURI_LOG_E("IR", "RX timeout timer is not available");
        ir_rx_cleanup_start_failure(true);
        return false;
    }

    ir_rx.rearm_done_semaphore = xSemaphoreCreateBinary();
    if(!ir_rx.rearm_done_semaphore) {
        FURI_LOG_E("IR", "RX rearm semaphore allocation failed");
        ir_rx_cleanup_start_failure(true);
        return false;
    }
    BaseType_t task_created = xTaskCreate(
        ir_rx_rearm_task,
        "ir_rx_rearm",
        IR_RX_REARM_TASK_STACK_SIZE,
        NULL,
        IR_RX_REARM_TASK_PRIORITY,
        &ir_rx.rearm_task_handle);
    if(task_created != pdPASS) {
        FURI_LOG_E("IR", "RX rearm task creation failed");
        ir_rx_cleanup_start_failure(true);
        return false;
    }

    ir_state_advance(InfraredStateStartingRx, InfraredStateAsyncRx);

    /* Start receiving */
    err = rmt_receive(ir_rx.channel, ir_rx.symbols, ir_rx.symbols_bytes, &ir_rx.rx_config);
    if(err != ESP_OK) {
        FURI_LOG_E("IR", "RX start failed: %d", err);
        ir_state_set(InfraredStateStartingRx);
        ir_rx_cleanup_start_failure(true);
        return false;
    }
    FURI_LOG_I("IR", "RX started (timeout=%lu us)", (unsigned long)ir_rx.timeout_us);
    return true;
}

void furi_hal_infrared_async_rx_stop(void) {
    furi_check(ir_state == InfraredStateAsyncRx);

    TaskHandle_t rearm_task = ir_rx.rearm_task_handle;
    if(rearm_task) {
        xTaskNotify(rearm_task, IR_RX_REARM_NOTIFY_EXIT, eSetBits);
        xSemaphoreTake(ir_rx.rearm_done_semaphore, portMAX_DELAY);
    }

    /* The silence-timer ISR (state is still AsyncRx) reads rearm_task_handle and
     * notifies that task. ir_rx_delete_timeout_timer() detaches the timer and waits
     * for an ISR already running on the other core, so it has to finish before the
     * task is deleted below. The task is parked by now, so it cannot be inside
     * ir_rx_apply_timeout_update() using the timer either. */
    ir_rx_delete_timeout_timer();

    const bool channel_gone = ir_rx_destroy_channel(true);

    if(rearm_task) {
        vTaskDelete(rearm_task);
        ir_rx.rearm_task_handle = NULL;
    }
    if(ir_rx.rearm_done_semaphore) {
        vSemaphoreDelete(ir_rx.rearm_done_semaphore);
        ir_rx.rearm_done_semaphore = NULL;
    }

    /* If the channel could not be deleted the RMT ISR may still copy into the
     * symbol buffer, so freeing it would be a use-after-free. Keep both; the
     * next rx_start() retries the teardown and then reuses the buffer. */
    if(channel_gone) {
        ir_rx_free_symbols();
    }
    ir_rx.timeout_us = 0;
    ir_rx.capture_callback = NULL;
    ir_rx.capture_context = NULL;
    ir_rx.timeout_callback = NULL;
    ir_rx.timeout_context = NULL;
    ir_rx.frame_end_callback = NULL;
    ir_rx.frame_end_context = NULL;
    ir_rx.error_callback = NULL;
    ir_rx.error_context = NULL;
    ir_state_set(InfraredStateIdle);
}

void furi_hal_infrared_async_rx_set_timeout(uint32_t timeout_us) {
    if(ir_state != InfraredStateIdle && ir_state != InfraredStateStartingRx) {
        FURI_LOG_W("IR", "set_timeout ignored: RX is not idle or starting");
        return;
    }

    ir_rx.timeout_us = timeout_us;

    ir_rx_delete_timeout_timer();

    if(timeout_us == 0) return;

    if(ir_rx_timer_orphan) {
        /* delete_timeout_timer() just retried it; creating another timer on top
         * would only leak more of the few hardware timers. rx_start() then fails
         * cleanly because the requested timeout timer is missing. */
        FURI_LOG_E("IR", "RX timer unavailable: earlier timer could not be deleted");
        return;
    }

    /* Create a one-shot alarm timer. Config structs must be in internal RAM
     * (gptimer ISR-IRAM-safe API rejects PSRAM-resident arguments). */
    memset(&ir_rx_timer_config, 0, sizeof(ir_rx_timer_config));
    ir_rx_timer_config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    ir_rx_timer_config.direction = GPTIMER_COUNT_UP;
    ir_rx_timer_config.resolution_hz = 1000000; /* 1 MHz = 1us */

    gptimer_handle_t new_timer = NULL;
    esp_err_t err = gptimer_new_timer(&ir_rx_timer_config, &new_timer);
    if(err != ESP_OK) {
        FURI_LOG_E("IR", "RX timer creation failed: %d", err);
        return;
    }

    memset(&ir_rx_alarm_config, 0, sizeof(ir_rx_alarm_config));
    ir_rx_alarm_config.alarm_count = timeout_us;
    ir_rx_alarm_config.reload_count = 0;
    ir_rx_alarm_config.flags.auto_reload_on_alarm = false;
    err = gptimer_set_alarm_action(new_timer, &ir_rx_alarm_config);
    if(err != ESP_OK) {
        FURI_LOG_E("IR", "RX timer alarm setup failed: %d", err);
        (void)gptimer_del_timer(new_timer);
        return;
    }

    memset(&ir_rx_timer_cbs, 0, sizeof(ir_rx_timer_cbs));
    ir_rx_timer_cbs.on_alarm = ir_rx_timeout_isr;
    err = gptimer_register_event_callbacks(new_timer, &ir_rx_timer_cbs, NULL);
    if(err != ESP_OK) {
        FURI_LOG_E("IR", "RX timer callback setup failed: %d", err);
        (void)gptimer_del_timer(new_timer);
        return;
    }
    err = gptimer_enable(new_timer);
    if(err != ESP_OK) {
        FURI_LOG_E("IR", "RX timer enable failed: %d", err);
        (void)gptimer_del_timer(new_timer);
        return;
    }
    portENTER_CRITICAL(&ir_rx_timer_lock);
    ir_rx.timeout_timer = new_timer;
    ir_rx.timeout_timer_running = false;
    ir_rx.timeout_timer_should_run = false;
    ir_rx.timeout_timer_restart_pending = false;
    ir_rx.timeout_alarm_armed = false;
    ir_rx.timeout_generation = 0;
    portEXIT_CRITICAL(&ir_rx_timer_lock);
    /* Timer will be started when first edge is received */
}

void furi_hal_infrared_async_rx_set_capture_isr_callback(
    FuriHalInfraredRxCaptureCallback callback,
    void* ctx) {
    furi_check(ir_state == InfraredStateStartingRx);
    ir_rx.capture_callback = callback;
    ir_rx.capture_context = ctx;
}

void furi_hal_infrared_async_rx_set_timeout_isr_callback(
    FuriHalInfraredRxTimeoutCallback callback,
    void* ctx) {
    furi_check(ir_state == InfraredStateStartingRx);
    ir_rx.timeout_callback = callback;
    ir_rx.timeout_context = ctx;
}

void furi_hal_infrared_async_rx_set_frame_end_isr_callback(
    FuriHalInfraredRxFrameEndCallback callback,
    void* ctx) {
    furi_check(ir_state == InfraredStateStartingRx);
    ir_rx.frame_end_callback = callback;
    ir_rx.frame_end_context = ctx;
}

void furi_hal_infrared_async_rx_set_error_callback(
    FuriHalInfraredRxErrorCallback callback,
    void* ctx) {
    furi_check(ir_state == InfraredStateStartingRx);
    ir_rx.error_callback = callback;
    ir_rx.error_context = ctx;
}

/* ---- TX Implementation ---- */

/**
 * TX strategy: We use a simple loop in a FreeRTOS task context.
 * The data_callback provides (duration_us, level) pairs.
 * We build RMT symbols and transmit them in batches.
 *
 * The STM32 uses DMA double-buffering with ISR callbacks;
 * on ESP32 we use the RMT encoder infrastructure with a custom
 * "IR TX encoder" that pulls data from the callback.
 */

/* A raw signal can hold up to MAX_TIMINGS_AMOUNT (1024) timings = 512 RMT
 * symbols. The whole packet MUST be sent in a single rmt_transmit() call:
 * splitting it into several transactions makes the RMT channel go idle
 * between them, inserting timing gaps mid-frame that corrupt the waveform so
 * the receiver no longer recognises it (raw/learned signals only — decoded
 * protocol packets are short). The RMT driver does internal ping-pong refill
 * from this buffer, so one transaction is gapless even though the buffer is
 * larger than mem_block_symbols. */


#define IR_RMT_MAX_DURATION 32767

typedef struct {
    rmt_symbol_word_t* symbols;
    size_t capacity; /* in symbols */
    size_t count; /* completed symbols */
    bool half; /* symbols[count] already holds duration0 */
} IrTxEmitter;

static bool ir_tx_emit_phase(IrTxEmitter* e, uint32_t* duration, bool level) {
    if(*duration == 0) *duration = 1; /* RMT reads a zero duration as end-of-frame */

    while(*duration > 0) {
        if(e->count >= e->capacity) return false;

        const uint32_t chunk =
            *duration > IR_RMT_MAX_DURATION ? IR_RMT_MAX_DURATION : *duration;
        *duration -= chunk;

        if(!e->half) {
            e->symbols[e->count].duration0 = chunk;
            e->symbols[e->count].level0 = level ? 1 : 0;
            e->half = true;
        } else {
            e->symbols[e->count].duration1 = chunk;
            e->symbols[e->count].level1 = level ? 1 : 0;
            e->count++;
            e->half = false;
        }
    }

    return true;
}

static void ir_tx_emit_flush(IrTxEmitter* e) {
    if(!e->half) return;
    e->symbols[e->count].duration1 = 1;
    e->symbols[e->count].level1 = 0;
    e->count++;
    e->half = false;
}

static void ir_tx_task(void* arg) {
    (void)arg;

    rmt_symbol_word_t* symbols =
        heap_caps_malloc(
            IR_TX_MAX_SYMBOLS * sizeof(rmt_symbol_word_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!symbols) {
        FURI_LOG_E("IR", "TX symbol buffer allocation failed");
        if(ir_tx.error_callback) {
            ir_tx.error_callback(ir_tx.error_context);
        }
        ir_state_set(InfraredStateAsyncTxStopped);
        if(ir_tx.done_semaphore) {
            xSemaphoreGive(ir_tx.done_semaphore);
        }
        vTaskDelete(NULL);
        return;
    }
    rmt_transmit_config_t tx_config = {
        .loop_count = 0, /* no loop, single shot */
        .flags = {
            .eot_level = 0, /* idle low after transmission */
        },
    };

    bool running = true;
    while(running) {
        IrTxEmitter emitter = {
            .symbols = symbols,
            .capacity = IR_TX_MAX_SYMBOLS,
            .count = 0,
            .half = false,
        };
        bool packet_end = false;
        bool last_packet = false;
        bool overflow = false;
        bool transmission_failed = false;

        while(!packet_end) {
            uint32_t duration = 0;
            bool level = false;
            const FuriHalInfraredTxGetDataState state =
                ir_tx.data_callback(ir_tx.data_context, &duration, &level);

            if(state != FuriHalInfraredTxGetDataStateOk &&
               state != FuriHalInfraredTxGetDataStateDone &&
               state != FuriHalInfraredTxGetDataStateLastDone) {
                FURI_LOG_E("IR", "TX data callback returned invalid state: %d", state);
                transmission_failed = true;
                last_packet = true;
                running = false;
                break;
            }

            /* Zero duration is reserved for the LastDone end marker. Accepting
             * it as data would silently turn a corrupt/invalid timing into 1 us. */
            if(duration == 0 && state != FuriHalInfraredTxGetDataStateLastDone) {
                FURI_LOG_E("IR", "TX data callback returned zero duration for data");
                transmission_failed = true;
                last_packet = true;
                running = false;
                break;
            }

            if(state == FuriHalInfraredTxGetDataStateLastDone && duration == 0) {
                last_packet = true;
                break;
            }

            if(!ir_tx_emit_phase(&emitter, &duration, level)) {
                FURI_LOG_E(
                    "IR",
                    "TX symbol buffer overflow mid-packet (%u/%u): "
                    "signal exceeds configured transmit capacity; aborting safe",
                    (unsigned)emitter.count,
                    (unsigned)emitter.capacity);
                overflow = true;
                break;
            }

            if(state == FuriHalInfraredTxGetDataStateDone) {
                packet_end = true;
            } else if(state == FuriHalInfraredTxGetDataStateLastDone) {
                packet_end = true;
                last_packet = true;
            }
        }

        if(!overflow && !transmission_failed) {
            ir_tx_emit_flush(&emitter);
            const size_t sym_count = emitter.count;

            if(sym_count > 0) {
                /* Transmit the whole packet in a single (gapless) transaction */
                esp_err_t transmit_result = rmt_transmit(
                    ir_tx.channel, ir_tx.copy_encoder,
                    symbols, sym_count * sizeof(rmt_symbol_word_t),
                    &tx_config);
                if(transmit_result != ESP_OK) {
                    FURI_LOG_E("IR", "TX submission failed: %d", transmit_result);
                    transmission_failed = true;
                    last_packet = true;
                    running = false;
                } else {
                    /* Wait for transmission to complete */
                    esp_err_t wait_result =
                        rmt_tx_wait_all_done(ir_tx.channel, 5000);
                    if(wait_result != ESP_OK) {
                        FURI_LOG_E("IR", "TX completion wait failed: %d", wait_result);
                        esp_err_t disable_result = rmt_disable(ir_tx.channel);
                        if(disable_result != ESP_OK) {
                            FURI_LOG_E("IR", "TX channel disable failed: %d", disable_result);
                            /* Releasing symbols while RMT may still reference
                             * them would create a use-after-free. */
                            furi_crash();
                        }
                        ir_tx.channel_disabled = true;
                        transmission_failed = true;
                        last_packet = true;
                        running = false;
                    }
                }
            }

            if(packet_end && !transmission_failed && ir_tx.signal_sent_callback) {
                ir_tx.signal_sent_callback(ir_tx.signal_sent_context);
            }
        } else if(overflow && ir_tx.overflow_callback) {
            ir_tx.overflow_callback(ir_tx.overflow_context);
        }

        if(transmission_failed && ir_tx.error_callback) {
            ir_tx.error_callback(ir_tx.error_context);
        }

        /* Check stop conditions */
        if(overflow || last_packet || ir_state == InfraredStateAsyncTxStopReq) {
            running = false;
        }
    }

    heap_caps_free(symbols);

    ir_state_set(InfraredStateAsyncTxStopped);
    if(ir_tx.done_semaphore) {
        xSemaphoreGive(ir_tx.done_semaphore);
    }

    vTaskDelete(NULL);
}

bool furi_hal_infrared_async_tx_start(uint32_t freq, float duty_cycle) {
    furi_check(ir_state == InfraredStateStartingTx);
    furi_check(ir_tx.data_callback != NULL);
    furi_check(freq >= INFRARED_MIN_FREQUENCY && freq <= INFRARED_MAX_FREQUENCY);
    furi_check(duty_cycle > 0.0f && duty_cycle <= 1.0f);
#if !BOARD_HAS_IR
    FURI_LOG_E("IR", "Board has no IR support");
    return false;
#endif

    ir_tx.carrier_freq = freq;
    ir_tx.duty_cycle = duty_cycle;

    gpio_num_t tx_gpio;
    if(!ir_tx_acquire_output(ir_tx_output, &tx_gpio)) {
        return false;
    }

    /* Create RMT TX channel with carrier modulation */
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RMT_TX_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
        .gpio_num = tx_gpio,
        .flags = {
            .invert_out = false,
            .with_dma = false,
        },
    };
    esp_err_t err = rmt_new_tx_channel(&tx_chan_config, &ir_tx.channel);
    if(err != ESP_OK) {
        FURI_LOG_E("IR", "TX channel creation failed: %d", err);
        ir_tx_release_output();
        return false;
    }

    /* Apply carrier modulation */
    rmt_carrier_config_t carrier_config = {
        .frequency_hz = freq,
        .duty_cycle = duty_cycle,
        .flags = {
            .polarity_active_low = false,
            .always_on = false, /* Only modulate during mark (level=1) */
        },
    };
    err = rmt_apply_carrier(ir_tx.channel, &carrier_config);
    if(err != ESP_OK) {
        FURI_LOG_E("IR", "TX carrier setup failed: %d", err);
        (void)rmt_del_channel(ir_tx.channel);
        ir_tx.channel = NULL;
        ir_tx_release_output();
        return false;
    }

    /* Create a copy encoder (pass-through, symbols already built) */
    rmt_copy_encoder_config_t copy_enc_config = {};
    err = rmt_new_copy_encoder(&copy_enc_config, &ir_tx.copy_encoder);
    if(err != ESP_OK) {
        FURI_LOG_E("IR", "TX encoder creation failed: %d", err);
        (void)rmt_del_channel(ir_tx.channel);
        ir_tx.channel = NULL;
        ir_tx_release_output();
        return false;
    }

    err = rmt_enable(ir_tx.channel);
    if(err != ESP_OK) {
        FURI_LOG_E("IR", "TX channel enable failed: %d", err);
        (void)rmt_del_encoder(ir_tx.copy_encoder);
        ir_tx.copy_encoder = NULL;
        (void)rmt_del_channel(ir_tx.channel);
        ir_tx.channel = NULL;
        ir_tx_release_output();
        return false;
    }

    /* Create synchronization semaphore (raw FreeRTOS, not Furi — tx task is xTaskCreate) */
    ir_tx.done_semaphore = xSemaphoreCreateBinary();
    if(!ir_tx.done_semaphore) {
        FURI_LOG_E("IR", "TX completion semaphore allocation failed");
        (void)rmt_disable(ir_tx.channel);
        (void)rmt_del_encoder(ir_tx.copy_encoder);
        (void)rmt_del_channel(ir_tx.channel);
        ir_tx.copy_encoder = NULL;
        ir_tx.channel = NULL;
        ir_tx_release_output();
        return false;
    }
    ir_tx.channel_disabled = false;

    ir_state_advance(InfraredStateStartingTx, InfraredStateAsyncTx);

    /* Start TX task */
    BaseType_t task_created = xTaskCreate(ir_tx_task, "ir_tx", 4096, NULL, 15, NULL);
    if(task_created != pdPASS) {
        vSemaphoreDelete(ir_tx.done_semaphore);
        ir_tx.done_semaphore = NULL;
        (void)rmt_disable(ir_tx.channel);
        (void)rmt_del_encoder(ir_tx.copy_encoder);
        (void)rmt_del_channel(ir_tx.channel);
        ir_tx.channel = NULL;
        ir_tx.copy_encoder = NULL;
        ir_state_set(InfraredStateStartingTx);
        FURI_LOG_E("IR", "TX task creation failed");
        ir_tx_release_output();
        return false;
    }
    return true;
}

void furi_hal_infrared_async_tx_wait_termination(void) {
    furi_check(ir_state >= InfraredStateAsyncTx);
    furi_check(ir_state < InfraredStateMAX);

    /* Wait for TX task to finish */
    xSemaphoreTake(ir_tx.done_semaphore, portMAX_DELAY);

    /* Cleanup */
    if(!ir_tx.channel_disabled) {
        ESP_ERROR_CHECK(rmt_disable(ir_tx.channel));
    }
    rmt_del_encoder(ir_tx.copy_encoder);
    rmt_del_channel(ir_tx.channel);
    ir_tx.channel = NULL;
    ir_tx.copy_encoder = NULL;
    ir_tx_release_output();

    vSemaphoreDelete(ir_tx.done_semaphore);
    ir_tx.done_semaphore = NULL;

    ir_tx.data_callback = NULL;
    ir_tx.data_context = NULL;
    ir_tx.signal_sent_callback = NULL;
    ir_tx.signal_sent_context = NULL;
    ir_tx.overflow_callback = NULL;
    ir_tx.overflow_context = NULL;
    ir_tx.error_callback = NULL;
    ir_tx.error_context = NULL;
    ir_state_set(InfraredStateIdle);
}

void furi_hal_infrared_async_tx_stop(void) {
    furi_check(ir_state >= InfraredStateAsyncTx);
    furi_check(ir_state < InfraredStateMAX);

    ir_state_request_tx_stop();

    furi_hal_infrared_async_tx_wait_termination();
}

void furi_hal_infrared_async_tx_set_data_isr_callback(
    FuriHalInfraredTxGetDataISRCallback callback,
    void* context) {
    furi_check(ir_state == InfraredStateStartingTx);
    ir_tx.data_callback = callback;
    ir_tx.data_context = context;
}

void furi_hal_infrared_async_tx_set_signal_sent_isr_callback(
    FuriHalInfraredTxSignalSentISRCallback callback,
    void* context) {
    furi_check(ir_state == InfraredStateStartingTx);
    ir_tx.signal_sent_callback = callback;
    ir_tx.signal_sent_context = context;
}

void furi_hal_infrared_async_tx_set_overflow_isr_callback(
    FuriHalInfraredTxOverflowISRCallback callback,
    void* context) {
    furi_check(ir_state == InfraredStateStartingTx);
    ir_tx.overflow_callback = callback;
    ir_tx.overflow_context = context;
}

void furi_hal_infrared_async_tx_set_error_callback(
    FuriHalInfraredTxErrorCallback callback,
    void* context) {
    furi_check(ir_state == InfraredStateStartingTx);
    ir_tx.error_callback = callback;
    ir_tx.error_context = context;
}

bool furi_hal_infrared_is_busy(void) {
    return ir_state != InfraredStateIdle;
}

bool furi_hal_infrared_is_available(void) {
#if BOARD_HAS_IR
    return true;
#else
    return false;
#endif
}

FuriHalInfraredTxPin furi_hal_infrared_detect_tx_output(void) {
    /* No module-detect circuit on these boards: "Detect" means internal. */
    return FuriHalInfraredTxPinInternal;
}

bool furi_hal_infrared_is_tx_output_supported(FuriHalInfraredTxPin tx_pin) {
    switch(tx_pin) {
    case FuriHalInfraredTxPinInternal:
        return IR_TX_INT_SUPPORTED;
    case FuriHalInfraredTxPinExtPA7:
        return IR_TX_EXT_SUPPORTED;
    default:
        return false;
    }
}

void furi_hal_infrared_set_tx_output(FuriHalInfraredTxPin tx_pin) {
    if(!furi_hal_infrared_is_tx_output_supported(tx_pin)) {
        if(tx_pin != FuriHalInfraredTxPinInternal) {
            FURI_LOG_W("IR", "TX output %d not available on this board; using internal", tx_pin);
        }
        tx_pin = FuriHalInfraredTxPinInternal;
    }
    ir_tx_output = tx_pin;
}

FuriHalInfraredTxPin furi_hal_infrared_get_tx_output(void) {
    return ir_tx_output;
}

bool furi_hal_infrared_is_tx_output_free(void) {
#if IR_TX_EXT_SUPPORTED
    if(ir_tx_output != FuriHalInfraredTxPinExtPA7) return true;

    const char* owner = furi_hal_gpio_get_reservation(&ir_tx_ext_pin);
    if(owner) return owner == IR_TX_EXT_OWNER;
    return !furi_hal_gpio_is_output(&ir_tx_ext_pin);
#else
    return true;
#endif
}
