/**
 * @file furi_hal_infrared.c
 * ESP32 Infrared HAL implementation using RMT peripheral.
 *
 * RX: RMT receiver captures edges, a GPTimer provides silence timeout.
 * TX: RMT transmitter with carrier modulation, fed via encode callback.
 */

#include <furi_hal_infrared.h>
#include <furi.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <driver/rmt_tx.h>
#include <driver/rmt_rx.h>
#include <driver/rmt_encoder.h>
#include <driver/gptimer.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>

#include BOARD_INCLUDE

/* ---- Configuration ---- */

#if BOARD_HAS_IR
#define IR_TX_GPIO_INTERNAL BOARD_PIN_IR_TX
#define IR_RX_GPIO_INTERNAL BOARD_PIN_IR_RX
#else
#define IR_TX_GPIO_INTERNAL GPIO_NUM_NC
#define IR_RX_GPIO_INTERNAL GPIO_NUM_NC
#endif

/* External IR module pins. Boards without a dedicated pin for one fall back
 * to the corresponding internal pin, so selecting "External" in the app is a
 * harmless no-op there instead of a compile error. */
#ifdef BOARD_PIN_IR_TX_EXT
#define IR_TX_GPIO_EXTERNAL BOARD_PIN_IR_TX_EXT
#else
#define IR_TX_GPIO_EXTERNAL IR_TX_GPIO_INTERNAL
#endif

#ifdef BOARD_PIN_IR_RX_EXT
#define IR_RX_GPIO_EXTERNAL BOARD_PIN_IR_RX_EXT
#else
#define IR_RX_GPIO_EXTERNAL IR_RX_GPIO_INTERNAL
#endif

/* Pin actually used for the next/current TX/RX session; switched by
 * furi_hal_infrared_set_tx_output() / furi_hal_infrared_set_rx_input(). */
static int ir_tx_gpio_active = IR_TX_GPIO_INTERNAL;
static int ir_rx_gpio_active = IR_RX_GPIO_INTERNAL;

#define IR_RMT_RX_MEM_BLOCK_SYMBOLS 128
#define IR_RMT_RX_RESOLUTION_HZ     1000000 /* 1 MHz = 1 us per tick */
#define IR_RMT_TX_RESOLUTION_HZ     1000000 /* 1 MHz = 1 us per tick */
#define IR_RMT_RX_MAX_SYMBOLS       1024

/* When the RX line returns to idle, RMT ends the burst and records the
 * terminating space as a zero-duration "end marker" — so the long inter-frame
 * space that trails every burst is never delivered to the decoder. The STM32
 * capture path delivers that space naturally (when the next burst's leading
 * edge arrives), and min_split_time protocols (NEC, Samsung, RC5/6, SIRC,
 * Kaseikyo, RCA, Pioneer) rely on it to finish decoding mid-stream. Without it
 * they can only complete via the silence timeout — which never fires while a
 * button is HELD, because the ~110 ms repeat bursts keep restarting it, so a
 * held remote is never learned. We therefore synthesise the trailing space at
 * the end of each idle-terminated burst. Its duration must exceed the largest
 * protocol min_split_time (Pioneer = 26000 us) yet stay below NEC's repeat
 * pause max (150000 us) and the silence timeout, so 50 ms is a safe choice. */
#define IR_RX_FRAME_END_SPACE_US 50000

/* ---- State ---- */

typedef enum {
    InfraredStateIdle,
    InfraredStateAsyncRx,
    InfraredStateAsyncTx,
    InfraredStateAsyncTxStopReq,
    InfraredStateAsyncTxStopped,
    InfraredStateMAX,
} InfraredState;

/* RX state */
static struct {
    rmt_channel_handle_t channel;
    rmt_receive_config_t rx_config;
    rmt_symbol_word_t symbols[IR_RMT_RX_MAX_SYMBOLS];
    FuriHalInfraredRxCaptureCallback capture_callback;
    void* capture_context;
    FuriHalInfraredRxTimeoutCallback timeout_callback;
    void* timeout_context;
    gptimer_handle_t timeout_timer;
    uint32_t timeout_us;
} ir_rx;

/* TX state */
static struct {
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t copy_encoder;
    FuriHalInfraredTxGetDataISRCallback data_callback;
    void* data_context;
    FuriHalInfraredTxSignalSentISRCallback signal_sent_callback;
    void* signal_sent_context;
    SemaphoreHandle_t done_semaphore;
    uint32_t carrier_freq;
    float duty_cycle;
} ir_tx;

static volatile InfraredState ir_state = InfraredStateIdle;

/* ---- RX Implementation ---- */

static void ir_rx_restart_timeout(void) {
    if(ir_rx.timeout_timer) {
        gptimer_stop(ir_rx.timeout_timer);
        gptimer_set_raw_count(ir_rx.timeout_timer, 0);
        gptimer_start(ir_rx.timeout_timer);
    }
}

static bool IRAM_ATTR ir_rx_timeout_isr(gptimer_handle_t timer,
                                         const gptimer_alarm_event_data_t* edata,
                                         void* user_ctx) {
    (void)timer;
    (void)edata;
    (void)user_ctx;
    gptimer_stop(timer);
    if(ir_rx.timeout_callback) {
        ir_rx.timeout_callback(ir_rx.timeout_context);
    }
    return false; /* no high-priority task woken */
}

static bool IRAM_ATTR ir_rmt_rx_done_callback(rmt_channel_handle_t channel,
                                                const rmt_rx_done_event_data_t* edata,
                                                void* user_ctx) {
    (void)channel;
    (void)user_ctx;

    bool last_level_mark = false; /* level of the last edge we delivered */
    bool any_edge = false;

    /* Process received symbols and call capture callback for each edge */
    for(size_t i = 0; i < edata->num_symbols; i++) {
        const rmt_symbol_word_t* sym = &edata->received_symbols[i];

        /* Each RMT symbol has two phases: duration0/level0 then duration1/level1.
         * invert_in=true on the RX channel already converts the active-low IR
         * receiver output to logical mark=1/space=0, so pass sym->level through
         * unchanged. Negating here would drop the leader mark via the worker's
         * "skip first space" rule. */
        if(sym->duration0 > 0 && ir_rx.capture_callback) {
            ir_rx.capture_callback(
                ir_rx.capture_context, sym->level0, sym->duration0);
            ir_rx_restart_timeout();
            last_level_mark = sym->level0;
            any_edge = true;
        }
        if(sym->duration1 > 0 && ir_rx.capture_callback) {
            ir_rx.capture_callback(
                ir_rx.capture_context, sym->level1, sym->duration1);
            ir_rx_restart_timeout();
            last_level_mark = sym->level1;
            any_edge = true;
        }
    }

    /* A burst always ends on a Mark (the trailing Space is the idle that
     * terminated reception, dropped above as a zero-duration end marker).
     * Deliver that trailing Space synthetically so min_split_time protocols
     * decode immediately, instead of waiting for a silence timeout that never
     * arrives while a held remote keeps repeating. See IR_RX_FRAME_END_SPACE_US.
     * Decoded protocols consume it (the worker resets its timing count, so no
     * stray raw timing); raw signals just gain a trailing gap. */
    if(any_edge && last_level_mark && ir_rx.capture_callback) {
        ir_rx.capture_callback(ir_rx.capture_context, false, IR_RX_FRAME_END_SPACE_US);
        ir_rx_restart_timeout();
    }

    /* Re-start receiving for next burst */
    if(ir_state == InfraredStateAsyncRx) {
        rmt_receive(channel, ir_rx.symbols, sizeof(ir_rx.symbols), &ir_rx.rx_config);
    }

    return false;
}

void furi_hal_infrared_async_rx_start(void) {
    furi_check(ir_state == InfraredStateIdle);
#if !BOARD_HAS_IR
    FURI_LOG_E("IR", "Board has no IR support");
    return;
#endif

    /* Fully reset the pin before handing it to RMT. Pins that default to a
     * special IOMUX function at boot (e.g. GPIO43/44 = U0TXD/U0RXD on
     * ESP32-S3) can retain pull-up/down and function-select state that the
     * RMT driver's own gpio_func_sel() doesn't clear. */
    gpio_reset_pin((gpio_num_t)ir_rx_gpio_active);

    /* Configure RMT RX channel */
    rmt_rx_channel_config_t rx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RMT_RX_RESOLUTION_HZ,
        .mem_block_symbols = IR_RMT_RX_MEM_BLOCK_SYMBOLS,
        .gpio_num = ir_rx_gpio_active,
        .flags = {
            .invert_in = true, /* IR receiver module output is active-low */
            .with_dma = false,
        },
    };
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_chan_config, &ir_rx.channel));

    /* Register RX done callback */
    rmt_rx_event_callbacks_t rx_cbs = {
        .on_recv_done = ir_rmt_rx_done_callback,
    };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(ir_rx.channel, &rx_cbs, NULL));

    /* Enable channel */
    ESP_ERROR_CHECK(rmt_enable(ir_rx.channel));

    /* Configure receive parameters */
    ir_rx.rx_config.signal_range_min_ns = 1250;  /* glitch filter: ignore < 1.25us */
    /* RMT idle threshold: silence longer than this ends the frame.
       Max at 1 MHz resolution is ~32.7 ms (RMT_LL_MAX_IDLE_VALUE=32767).
       Use 20 ms default which covers all common IR protocols. */
    uint32_t max_ns = ir_rx.timeout_us > 0 ? ir_rx.timeout_us * 1000 : 20000 * 1000;
    if(max_ns > 32000 * 1000) max_ns = 32000 * 1000; /* clamp to RMT HW limit */
    ir_rx.rx_config.signal_range_max_ns = max_ns;
    ir_rx.rx_config.flags.en_partial_rx = false;

    ir_state = InfraredStateAsyncRx;

    /* Start receiving */
    ESP_ERROR_CHECK(rmt_receive(ir_rx.channel, ir_rx.symbols, sizeof(ir_rx.symbols), &ir_rx.rx_config));
}

void furi_hal_infrared_async_rx_stop(void) {
    furi_check(ir_state == InfraredStateAsyncRx);

    ir_state = InfraredStateIdle;

    rmt_disable(ir_rx.channel);
    rmt_del_channel(ir_rx.channel);
    ir_rx.channel = NULL;

    /* Stop and delete timeout timer if active */
    if(ir_rx.timeout_timer) {
        gptimer_stop(ir_rx.timeout_timer);
        gptimer_disable(ir_rx.timeout_timer);
        gptimer_del_timer(ir_rx.timeout_timer);
        ir_rx.timeout_timer = NULL;
    }
}

void furi_hal_infrared_async_rx_set_timeout(uint32_t timeout_us) {
    ir_rx.timeout_us = timeout_us;

    /* If RX is already running, create/update the GPTimer for timeout */
    if(ir_state == InfraredStateAsyncRx || ir_state == InfraredStateIdle) {
        /* Delete existing timer */
        if(ir_rx.timeout_timer) {
            gptimer_stop(ir_rx.timeout_timer);
            gptimer_disable(ir_rx.timeout_timer);
            gptimer_del_timer(ir_rx.timeout_timer);
            ir_rx.timeout_timer = NULL;
        }

        if(timeout_us == 0) return;

        /* Create a one-shot alarm timer */
        gptimer_config_t timer_config = {
            .clk_src = GPTIMER_CLK_SRC_DEFAULT,
            .direction = GPTIMER_COUNT_UP,
            .resolution_hz = 1000000, /* 1 MHz = 1us */
        };
        ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &ir_rx.timeout_timer));

        gptimer_alarm_config_t alarm_config = {
            .alarm_count = timeout_us,
            .reload_count = 0,
            .flags = {
                .auto_reload_on_alarm = false,
            },
        };
        ESP_ERROR_CHECK(gptimer_set_alarm_action(ir_rx.timeout_timer, &alarm_config));

        gptimer_event_callbacks_t timer_cbs = {
            .on_alarm = ir_rx_timeout_isr,
        };
        ESP_ERROR_CHECK(gptimer_register_event_callbacks(ir_rx.timeout_timer, &timer_cbs, NULL));
        ESP_ERROR_CHECK(gptimer_enable(ir_rx.timeout_timer));
        /* Timer will be started when first edge is received */
    }
}

void furi_hal_infrared_async_rx_set_capture_isr_callback(
    FuriHalInfraredRxCaptureCallback callback,
    void* ctx) {
    ir_rx.capture_callback = callback;
    ir_rx.capture_context = ctx;
}

void furi_hal_infrared_async_rx_set_timeout_isr_callback(
    FuriHalInfraredRxTimeoutCallback callback,
    void* ctx) {
    ir_rx.timeout_callback = callback;
    ir_rx.timeout_context = ctx;
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
#define IR_TX_MAX_SYMBOLS 600

#define IR_RMT_MAX_DURATION 32767

typedef struct {
    rmt_symbol_word_t* symbols;
    size_t capacity; /* in symbols */
    size_t count; /* completed symbols */
    bool half; /* symbols[count] already holds duration0 */
} IrTxEmitter;

static bool ir_tx_emit_phase(IrTxEmitter* e, uint32_t duration, bool level) {
    if(duration == 0) duration = 1; /* RMT reads a zero duration as end-of-frame */

    const size_t chunks = (duration + IR_RMT_MAX_DURATION - 1) / IR_RMT_MAX_DURATION;
    const size_t free_halves = (e->capacity - e->count) * 2 - (e->half ? 1 : 0);
    if(chunks > free_halves) return false;

    while(duration > 0) {
        const uint32_t chunk = duration > IR_RMT_MAX_DURATION ? IR_RMT_MAX_DURATION : duration;
        duration -= chunk;

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
        heap_caps_malloc(IR_TX_MAX_SYMBOLS * sizeof(rmt_symbol_word_t), MALLOC_CAP_8BIT);
    furi_check(symbols);
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

        while(!packet_end) {
            uint32_t duration = 0;
            bool level = false;

            const FuriHalInfraredTxGetDataState state =
                ir_tx.data_callback(ir_tx.data_context, &duration, &level);

            if(state == FuriHalInfraredTxGetDataStateLastDone && duration == 0) {
                last_packet = true;
                break;
            }

            if(!ir_tx_emit_phase(&emitter, duration, level)) {
                /* Buffer exhausted — send what we have (see IR_TX_MAX_SYMBOLS). */
                break;
            }

            if(state == FuriHalInfraredTxGetDataStateDone) {
                packet_end = true;
            } else if(state == FuriHalInfraredTxGetDataStateLastDone) {
                packet_end = true;
                last_packet = true;
            }
        }

        ir_tx_emit_flush(&emitter);
        const size_t sym_count = emitter.count;

        if(sym_count > 0) {
            /* Transmit the whole packet in a single (gapless) transaction */
            ESP_ERROR_CHECK(rmt_transmit(
                ir_tx.channel, ir_tx.copy_encoder,
                symbols, sym_count * sizeof(rmt_symbol_word_t),
                &tx_config));
            /* Wait for transmission to complete */
            ESP_ERROR_CHECK(rmt_tx_wait_all_done(ir_tx.channel, portMAX_DELAY));
        }

        if(packet_end) {
            /* Signal sent callback */
            if(ir_tx.signal_sent_callback) {
                ir_tx.signal_sent_callback(ir_tx.signal_sent_context);
            }
        }

        /* Check stop conditions */
        if(last_packet || ir_state == InfraredStateAsyncTxStopReq) {
            running = false;
        }
    }

    heap_caps_free(symbols);

    ir_state = InfraredStateAsyncTxStopped;
    if(ir_tx.done_semaphore) {
        xSemaphoreGive(ir_tx.done_semaphore);
    }

    vTaskDelete(NULL);
}

void furi_hal_infrared_async_tx_start(uint32_t freq, float duty_cycle) {
    furi_check(ir_state == InfraredStateIdle);
    furi_check(ir_tx.data_callback != NULL);
    furi_check(freq >= INFRARED_MIN_FREQUENCY && freq <= INFRARED_MAX_FREQUENCY);
    furi_check(duty_cycle > 0.0f && duty_cycle <= 1.0f);
#if !BOARD_HAS_IR
    FURI_LOG_E("IR", "Board has no IR support");
    return;
#endif

    ir_tx.carrier_freq = freq;
    ir_tx.duty_cycle = duty_cycle;

    /* Fully reset the pin before handing it to RMT -- see the matching
     * comment in furi_hal_infrared_async_rx_start(). */
    gpio_reset_pin((gpio_num_t)ir_tx_gpio_active);

    /* Create RMT TX channel with carrier modulation */
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RMT_TX_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
        .gpio_num = ir_tx_gpio_active,
        .flags = {
            .invert_out = false,
            .with_dma = false,
        },
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &ir_tx.channel));

    /* Apply carrier modulation */
    rmt_carrier_config_t carrier_config = {
        .frequency_hz = freq,
        .duty_cycle = duty_cycle,
        .flags = {
            .polarity_active_low = false,
            .always_on = false, /* Only modulate during mark (level=1) */
        },
    };
    ESP_ERROR_CHECK(rmt_apply_carrier(ir_tx.channel, &carrier_config));

    /* Create a copy encoder (pass-through, symbols already built) */
    rmt_copy_encoder_config_t copy_enc_config = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_enc_config, &ir_tx.copy_encoder));

    ESP_ERROR_CHECK(rmt_enable(ir_tx.channel));

    /* Create synchronization semaphore (raw FreeRTOS, not Furi — tx task is xTaskCreate) */
    ir_tx.done_semaphore = xSemaphoreCreateBinary();

    ir_state = InfraredStateAsyncTx;

    /* Start TX task */
    xTaskCreate(ir_tx_task, "ir_tx", 4096, NULL, 15, NULL);
}

void furi_hal_infrared_async_tx_wait_termination(void) {
    furi_check(ir_state >= InfraredStateAsyncTx);
    furi_check(ir_state < InfraredStateMAX);

    /* Wait for TX task to finish */
    xSemaphoreTake(ir_tx.done_semaphore, portMAX_DELAY);

    /* Cleanup */
    rmt_disable(ir_tx.channel);
    rmt_del_encoder(ir_tx.copy_encoder);
    rmt_del_channel(ir_tx.channel);
    ir_tx.channel = NULL;
    ir_tx.copy_encoder = NULL;

    vSemaphoreDelete(ir_tx.done_semaphore);
    ir_tx.done_semaphore = NULL;

    ir_state = InfraredStateIdle;
}

void furi_hal_infrared_async_tx_stop(void) {
    furi_check(ir_state >= InfraredStateAsyncTx);
    furi_check(ir_state < InfraredStateMAX);

    if(ir_state == InfraredStateAsyncTx) {
        ir_state = InfraredStateAsyncTxStopReq;
    }

    furi_hal_infrared_async_tx_wait_termination();
}

void furi_hal_infrared_async_tx_set_data_isr_callback(
    FuriHalInfraredTxGetDataISRCallback callback,
    void* context) {
    furi_check(ir_state == InfraredStateIdle);
    ir_tx.data_callback = callback;
    ir_tx.data_context = context;
}

void furi_hal_infrared_async_tx_set_signal_sent_isr_callback(
    FuriHalInfraredTxSignalSentISRCallback callback,
    void* context) {
    ir_tx.signal_sent_callback = callback;
    ir_tx.signal_sent_context = context;
}

bool furi_hal_infrared_is_busy(void) {
    return ir_state != InfraredStateIdle;
}

FuriHalInfraredTxPin furi_hal_infrared_detect_tx_output(void) {
    /* No external-module detection circuit on ESP32 boards -- default to
     * internal; the user can still pick External explicitly in settings. */
    return FuriHalInfraredTxPinInternal;
}

void furi_hal_infrared_set_tx_output(FuriHalInfraredTxPin tx_pin) {
    furi_check(ir_state == InfraredStateIdle);
    switch(tx_pin) {
    case FuriHalInfraredTxPinExtPA7:
        ir_tx_gpio_active = IR_TX_GPIO_EXTERNAL;
        break;
    case FuriHalInfraredTxPinInternal:
    default:
        ir_tx_gpio_active = IR_TX_GPIO_INTERNAL;
        break;
    }
    /* Also reset the pin we just switched AWAY from. A previous RMT session
     * on it left the GPIO matrix routed to that pin; without resetting it,
     * its pad can keep carrying the RMT output signal alongside (or instead
     * of) the newly selected pin. */
    gpio_reset_pin(
        (gpio_num_t)(ir_tx_gpio_active == IR_TX_GPIO_INTERNAL ? IR_TX_GPIO_EXTERNAL
                                                                : IR_TX_GPIO_INTERNAL));
}

void furi_hal_infrared_set_rx_input(FuriHalInfraredRxPin rx_pin) {
    furi_check(ir_state == InfraredStateIdle);
    switch(rx_pin) {
    case FuriHalInfraredRxPinExternal:
        ir_rx_gpio_active = IR_RX_GPIO_EXTERNAL;
        break;
    case FuriHalInfraredRxPinInternal:
    default:
        ir_rx_gpio_active = IR_RX_GPIO_INTERNAL;
        break;
    }
    /* See the matching comment in furi_hal_infrared_set_tx_output(). */
    gpio_reset_pin(
        (gpio_num_t)(ir_rx_gpio_active == IR_RX_GPIO_INTERNAL ? IR_RX_GPIO_EXTERNAL
                                                                : IR_RX_GPIO_INTERNAL));
}
