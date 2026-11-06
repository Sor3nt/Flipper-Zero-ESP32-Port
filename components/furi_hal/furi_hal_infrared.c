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

/* External TX pin (Infrared Settings > TX Pin: External). Only on boards that
 * define BOARD_PIN_IR_TX_EXT; every other board keeps the code paths below
 * marked "#else" unchanged. */
#if BOARD_HAS_IR && defined(BOARD_PIN_IR_TX_EXT)
#define IR_TX_EXT_SUPPORTED 1
#include <furi_hal_gpio.h>
#include <nvs.h>
#include <esp_log.h>
#include <esp_rom_gpio.h>
#include <soc/gpio_sig_map.h>
#if defined(BOARD_PIN_RFID_TX) && defined(BOARD_PIN_RFID_RX) && \
    (BOARD_PIN_IR_TX_EXT == BOARD_PIN_RFID_TX || BOARD_PIN_IR_TX_EXT == BOARD_PIN_RFID_RX)
/* The pin is one of the shared UART1 / RFID / NRF24 pads (T-Embed GPIO43). */
#define IR_TX_EXT_SHARED_PINS 1
#include <furi_hal_shared_pins.h>
#include <furi_hal_external_uart.h>
#include <esp_private/esp_gpio_reserve.h>
#if defined(BOARD_PIN_NRF24_CE) && (BOARD_PIN_NRF24_CE == BOARD_PIN_IR_TX_EXT) && \
    defined(BOARD_PIN_NRF24_CSN) && defined(BOARD_LCD_SPI_HOST)
#include "furi_hal_spi_bus.h"
#include <driver/spi_master.h>
#include <esp_rom_sys.h>
#define IR_TX_EXT_NRF24_CE 1
#else
#define IR_TX_EXT_NRF24_CE 0
#endif
#else
#define IR_TX_EXT_SHARED_PINS 0
#endif
#else
#define IR_TX_EXT_SUPPORTED 0
#endif

/* ---- Configuration ---- */

#if BOARD_HAS_IR
#define IR_TX_GPIO  BOARD_PIN_IR_TX
#define IR_RX_GPIO  BOARD_PIN_IR_RX
#else
#define IR_TX_GPIO  GPIO_NUM_NC
#define IR_RX_GPIO  GPIO_NUM_NC
#endif

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

/* The order matters: furi_hal_infrared_async_tx_wait_termination() accepts every
 * state from InfraredStateAsyncTx up to (excluding) InfraredStateMAX. */
typedef enum {
    InfraredStateIdle,
    InfraredStateClaimedTx, /* furi_hal_infrared_claim_tx() taken, TX not started yet */
    InfraredStateAsyncRx,
    InfraredStateStoppingRx, /* async_rx_stop() tearing the RX channel down */
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
    FuriHalInfraredTxErrorCallback error_callback;
    void* error_context;
    SemaphoreHandle_t done_semaphore;
    SemaphoreHandle_t packet_done; /* given by the RMT "transaction done" ISR, one per packet */
    uint32_t carrier_freq;
    float duty_cycle;
    bool channel_disabled; /* rmt_disable() already done (abort): do not repeat it */
    bool output_held; /* an output pad is owned (ext lease / LED pad) until teardown */
    TaskHandle_t claim_owner; /* the task that holds the claim (or runs the transmission) */
    volatile bool abort_requested; /* furi_hal_infrared_async_tx_abort() */
} ir_tx;

/* Every transition of ir_state goes through ir_state_lock, so two users can
 * never both "see" Idle and both start (the old furi_check(ir_state == Idle)
 * was a check-then-act race that reset the device instead of refusing). The
 * RX done ISR only reads it. */
static portMUX_TYPE ir_state_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile InfraredState ir_state = InfraredStateIdle;

static bool ir_state_transition(InfraredState from, InfraredState to) {
    bool done = false;
    portENTER_CRITICAL(&ir_state_lock);
    if(ir_state == from) {
        ir_state = to;
        done = true;
    }
    portEXIT_CRITICAL(&ir_state_lock);
    return done;
}

static void ir_state_set(InfraredState state) {
    portENTER_CRITICAL(&ir_state_lock);
    ir_state = state;
    portEXIT_CRITICAL(&ir_state_lock);
}

/* Why the last transmission did not start / was aborted (all boards). */
static volatile FuriHalInfraredTxError ir_tx_last_error = FuriHalInfraredTxErrorNone;

#if IR_TX_EXT_SUPPORTED
#define IR_EXT_TAG "IrTxExt"
/* Selected output, read once per transmission into ir_tx_output_active: a
 * selection change mid-transmission never mismatches claim and release. */
static volatile FuriHalInfraredTxPin ir_tx_output_cfg = FuriHalInfraredTxPinInternal;
static FuriHalInfraredTxPin ir_tx_output_active = FuriHalInfraredTxPinInternal;
static bool ir_tx_output_loaded = false;
static const char ir_tx_ext_owner[] = "Infrared";

#define IR_TX_NVS_NAMESPACE "furi_ir"
#define IR_TX_NVS_KEY       "tx_out"

/* Boot-time default saved by the Infrared settings. */
static void ir_tx_output_load(void) {
    if(ir_tx_output_loaded) return;
    ir_tx_output_loaded = true;
    nvs_handle_t nvs;
    if(nvs_open(IR_TX_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t value = 0;
        if(nvs_get_u8(nvs, IR_TX_NVS_KEY, &value) == ESP_OK &&
           value == FuriHalInfraredTxPinExtPA7) {
            ir_tx_output_cfg = FuriHalInfraredTxPinExtPA7;
        }
        nvs_close(nvs);
    }
}

/* Drive an IR TX pad low as a plain GPIO, detached from any peripheral
 * output: the LED stays dark instead of floating. */
static void ir_tx_pin_park_low(gpio_num_t pin) {
    gpio_set_level(pin, 0);
    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
}

#if IR_TX_EXT_SHARED_PINS
static bool ir_tx_ext_was_reserved = false;

#if IR_TX_EXT_NRF24_CE
static esp_err_t ir_tx_ext_nrf24_reg(spi_device_handle_t device, bool write, uint8_t* value) {
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length = 16,
        .tx_data = {write ? 0x20 : 0x00 /* CONFIG */, write ? *value : 0xFF},
    };
    gpio_set_level((gpio_num_t)BOARD_PIN_NRF24_CSN, 0);
    esp_rom_delay_us(1);
    const esp_err_t err = spi_device_polling_transmit(device, &t);
    gpio_set_level((gpio_num_t)BOARD_PIN_NRF24_CSN, 1);
    esp_rom_delay_us(1);
    if(err == ESP_OK && !write) *value = t.rx_data[1];
    return err;
}

/* The external pin is the NRF24 CE line. CE pulses at the carrier rate would
 * make a powered-up NRF24 transmit, so clear PWR_UP first (a powered-down
 * NRF24 ignores CE). Same approach as the BW16 guard: a temporary no-CS SPI
 * device with CSN driven by hand, removed again, so no CS routing outlives
 * this call (the pads are restored from the lease snapshot afterwards). */
static void ir_tx_ext_nrf24_power_down(void) {
    spi_device_interface_config_t cfg = {
        .clock_speed_hz = 1000000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    spi_device_handle_t device = NULL;
    furi_hal_spi_bus_lock();
    if(spi_bus_add_device(BOARD_LCD_SPI_HOST, &cfg, &device) == ESP_OK) {
        if(spi_device_acquire_bus(device, portMAX_DELAY) == ESP_OK) {
            gpio_set_level((gpio_num_t)BOARD_PIN_NRF24_CSN, 1);
            gpio_set_direction((gpio_num_t)BOARD_PIN_NRF24_CSN, GPIO_MODE_OUTPUT);
            uint8_t config = 0xFF;
            /* Bit 7 of CONFIG is reserved (0): 0xFF means no radio answered. */
            if(ir_tx_ext_nrf24_reg(device, false, &config) == ESP_OK && !(config & 0x80) &&
               (config & 0x02)) {
                config &= (uint8_t)~0x02;
                ir_tx_ext_nrf24_reg(device, true, &config);
            }
            spi_device_release_bus(device);
        }
        spi_bus_remove_device(device);
    }
    furi_hal_spi_bus_unlock();
}
#endif
#endif

/* Take the external pin for one transmission. Never blocks: a pin owned by
 * another function refuses the transmission instead. */
static bool ir_tx_ext_claim(void) {
#if IR_TX_EXT_SHARED_PINS
    /* A GPS / BW16 module listens on this pad in the external-UART profile. */
    if(furi_hal_external_uart_available()) return false;
    /* Lease held by RFID, the NRF24 app, USB-UART or BW16. */
    if(!furi_hal_shared_pins_acquire(ir_tx_ext_owner)) return false;
    if(!furi_hal_shared_pins_save(ir_tx_ext_owner)) {
        furi_hal_shared_pins_release(ir_tx_ext_owner);
        return false;
    }
    ir_tx_ext_was_reserved = esp_gpio_is_reserved(BIT64(BOARD_PIN_IR_TX_EXT));
#if IR_TX_EXT_NRF24_CE
    ir_tx_ext_nrf24_power_down();
#endif
    return true;
#else
    const GpioPin pin = {.port = NULL, .pin = BOARD_PIN_IR_TX_EXT};
    if(!furi_hal_gpio_reserve(&pin, ir_tx_ext_owner)) return false;
    /* Configured as an output by someone else (GPIO app, script, FAP)? */
    if(furi_hal_gpio_is_output(&pin)) {
        furi_hal_gpio_unreserve(&pin, ir_tx_ext_owner);
        return false;
    }
    return true;
#endif
}

/* Hand the pin back exactly as it was. Only after the RMT channel is gone. */
static void ir_tx_ext_release(void) {
#if IR_TX_EXT_SHARED_PINS
    /* Restores mux, level, direction and UART routing of the previous owner
     * (e.g. NRF24 CE output low). */
    furi_hal_shared_pins_restore(ir_tx_ext_owner);
    if(ir_tx_ext_was_reserved) esp_gpio_reserve(BIT64(BOARD_PIN_IR_TX_EXT));
    furi_hal_shared_pins_release(ir_tx_ext_owner);
#else
    const gpio_num_t gpio = (gpio_num_t)BOARD_PIN_IR_TX_EXT;
    esp_rom_gpio_connect_out_signal(gpio, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_direction(gpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode(gpio, GPIO_PULLDOWN_ONLY);
    const GpioPin pin = {.port = NULL, .pin = BOARD_PIN_IR_TX_EXT};
    furi_hal_gpio_unreserve(&pin, ir_tx_ext_owner);
#endif
}

#endif

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
    /* Atomic: a TX claim, a running TX or a second RX start is a caller bug. */
    furi_check(ir_state_transition(InfraredStateIdle, InfraredStateAsyncRx));
#if !BOARD_HAS_IR
    /* Stay in AsyncRx so the caller's async_rx_stop() is still a valid pair. */
    FURI_LOG_E("IR", "Board has no IR support");
    return;
#endif

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

    /* ir_state is already InfraredStateAsyncRx (claimed above), which the RX
     * done callback needs in order to re-arm itself. */

    /* Start receiving */
    ESP_ERROR_CHECK(rmt_receive(ir_rx.channel, ir_rx.symbols, sizeof(ir_rx.symbols), &ir_rx.rx_config));
}

void furi_hal_infrared_async_rx_stop(void) {
    furi_check(ir_state == InfraredStateAsyncRx);

    /* Leave AsyncRx first so the done callback stops re-arming the receiver
     * while the channel is torn down. The peripheral is only offered to a TX
     * claim again once the channel is gone (state set at the end). */
    ir_state_set(InfraredStateStoppingRx);

    if(ir_rx.channel) {
        rmt_disable(ir_rx.channel);
        rmt_del_channel(ir_rx.channel);
        ir_rx.channel = NULL;
    }

    /* Stop and delete timeout timer if active */
    if(ir_rx.timeout_timer) {
        gptimer_stop(ir_rx.timeout_timer);
        gptimer_disable(ir_rx.timeout_timer);
        gptimer_del_timer(ir_rx.timeout_timer);
        ir_rx.timeout_timer = NULL;
    }

    ir_state_set(InfraredStateIdle);
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

/* How often a running packet is checked for an abort request. */
#define IR_TX_ABORT_POLL_MS 20

/* The RMT driver calls this from its ISR when a transaction (= one packet) has been
 * sent completely. Waiting on this semaphore instead of polling rmt_tx_wait_all_done()
 * with a short timeout matters: every timeout of that function logs an error line. */
static bool IRAM_ATTR ir_rmt_tx_done_callback(
    rmt_channel_handle_t channel,
    const rmt_tx_done_event_data_t* edata,
    void* user_ctx) {
    (void)channel;
    (void)edata;
    (void)user_ctx;
    BaseType_t woken = pdFALSE;
    if(ir_tx.packet_done) xSemaphoreGiveFromISR(ir_tx.packet_done, &woken);
    return woken == pdTRUE;
}

/* Wait until the packet in flight has been sent. An abort request cuts it
 * short: only this task may disable the channel (a disable from another task
 * can land between two packets, and a transaction queued on a disabled channel
 * is never completed, so the next wait would hang for ever). The ESP-IDF RMT
 * driver stops the output at once on rmt_disable() and applies eot_level (low).
 * @return ESP_OK when the packet was sent or aborted, an error otherwise. */
static esp_err_t ir_tx_wait_packet(bool* aborted) {
    *aborted = false;
    for(;;) {
        if(xSemaphoreTake(ir_tx.packet_done, pdMS_TO_TICKS(IR_TX_ABORT_POLL_MS)) == pdTRUE) {
            /* Sent: recycle the finished transaction (returns at once). */
            return rmt_tx_wait_all_done(ir_tx.channel, 100);
        }
        if(ir_tx.abort_requested) {
            /* Stops the output and recycles the interrupted transaction, so the
             * flush below returns without waiting. The transaction may have just
             * finished: disable is valid in the enable and in the run state. */
            const esp_err_t disable_err = rmt_disable(ir_tx.channel);
            ir_tx.channel_disabled = true;
            *aborted = true;
            xSemaphoreTake(ir_tx.packet_done, 0); /* a done event racing the abort */
            if(disable_err != ESP_OK) return disable_err;
            return rmt_tx_wait_all_done(ir_tx.channel, 100);
        }
    }
}

static void ir_tx_task(void* arg) {
    (void)arg;

    rmt_symbol_word_t* symbols =
        heap_caps_malloc(IR_TX_MAX_SYMBOLS * sizeof(rmt_symbol_word_t), MALLOC_CAP_8BIT);
    rmt_transmit_config_t tx_config = {
        .loop_count = 0, /* no loop, single shot */
        .flags = {
            .eot_level = 0, /* idle low after transmission */
        },
    };

    bool failed = (symbols == NULL);
    bool running = !failed;

    /* One timing that did not fit into the previous packet: it is sent first in
     * the next one instead of being dropped (with its Done / LastDone state). */
    bool have_pending = false;
    uint32_t pending_duration = 0;
    bool pending_level = false;
    FuriHalInfraredTxGetDataState pending_state = FuriHalInfraredTxGetDataStateOk;

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
            FuriHalInfraredTxGetDataState state;

            if(have_pending) {
                have_pending = false;
                duration = pending_duration;
                level = pending_level;
                state = pending_state;
            } else {
                state = ir_tx.data_callback(ir_tx.data_context, &duration, &level);
            }

            if(state == FuriHalInfraredTxGetDataStateLastDone && duration == 0) {
                last_packet = true;
                break;
            }

            if(!ir_tx_emit_phase(&emitter, duration, level)) {
                if(emitter.count == 0 && !emitter.half) {
                    /* Longer than a whole empty buffer: it can never fit. Drop
                     * it (rather than loop for ever) but keep its end state. */
                    FURI_LOG_E("IR", "TX timing of %lu us does not fit a packet", (unsigned long)duration);
                    if(state == FuriHalInfraredTxGetDataStateDone) {
                        packet_end = true;
                    } else if(state == FuriHalInfraredTxGetDataStateLastDone) {
                        packet_end = true;
                        last_packet = true;
                    }
                    continue;
                }
                /* Buffer exhausted: send what we have and the rest next time
                 * (see IR_TX_MAX_SYMBOLS). */
                have_pending = true;
                pending_duration = duration;
                pending_level = level;
                pending_state = state;
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
        bool aborted = false;

        if(ir_tx.abort_requested) {
            /* Asked to cut the signal while this packet was being built: do not
             * start it. */
            aborted = true;
        } else if(sym_count > 0) {
            /* Transmit the whole packet in a single (gapless) transaction */
            xSemaphoreTake(ir_tx.packet_done, 0); /* no stale event of an earlier packet */
            esp_err_t err = rmt_transmit(
                ir_tx.channel,
                ir_tx.copy_encoder,
                symbols,
                sym_count * sizeof(rmt_symbol_word_t),
                &tx_config);
            if(err == ESP_OK) err = ir_tx_wait_packet(&aborted);
            if(err != ESP_OK) {
                FURI_LOG_E("IR", "TX failed: %s", esp_err_to_name(err));
                failed = true;
                break;
            }
        }

        /* An aborted packet was not sent completely: no "signal sent". */
        if(packet_end && !aborted) {
            if(ir_tx.signal_sent_callback) {
                ir_tx.signal_sent_callback(ir_tx.signal_sent_context);
            }
        }

        /* Check stop conditions */
        if(aborted || last_packet || ir_state == InfraredStateAsyncTxStopReq) {
            running = false;
        }
    }

    if(symbols) heap_caps_free(symbols);

    if(failed) {
        ir_tx_last_error = FuriHalInfraredTxErrorHardware;
        if(ir_tx.error_callback) {
            ir_tx.error_callback(ir_tx.error_context);
        }
    }

    ir_state_set(InfraredStateAsyncTxStopped);
    if(ir_tx.done_semaphore) {
        xSemaphoreGive(ir_tx.done_semaphore);
    }

    vTaskDelete(NULL);
}

/* End a transmission that never started: callers' wait_termination() /
 * stop() still find a finished transmission (see furi_hal_infrared_async_tx_start). */
static void ir_tx_refuse(FuriHalInfraredTxError error) {
    ir_tx_last_error = error;
    ir_tx.done_semaphore = xSemaphoreCreateBinary();
    furi_check(ir_tx.done_semaphore);
    ir_state_set(InfraredStateAsyncTxStopped);
    xSemaphoreGive(ir_tx.done_semaphore);
}

/* Delete the RMT channel (if any) and hand the pads back. Safe to repeat, and
 * to call with only some of the resources set up. Task context, after the TX
 * task is gone: no RMT ISR can still use the channel. The channel goes first,
 * the pad is handed back only afterwards. */
static void ir_tx_output_teardown(void) {
    if(ir_tx.channel) {
        if(!ir_tx.channel_disabled) {
            rmt_disable(ir_tx.channel);
            ir_tx.channel_disabled = true;
        }
        if(ir_tx.copy_encoder) {
            rmt_del_encoder(ir_tx.copy_encoder);
            ir_tx.copy_encoder = NULL;
        }
        const esp_err_t del_err = rmt_del_channel(ir_tx.channel);
        ir_tx.channel = NULL;
        if(del_err != ESP_OK) {
            FURI_LOG_E("IR", "RMT TX channel delete failed: %s", esp_err_to_name(del_err));
        }
    } else if(ir_tx.copy_encoder) {
        rmt_del_encoder(ir_tx.copy_encoder);
        ir_tx.copy_encoder = NULL;
    }
    /* Only after the channel is gone: its ISR is the one that gives this semaphore. */
    if(ir_tx.packet_done) {
        vSemaphoreDelete(ir_tx.packet_done);
        ir_tx.packet_done = NULL;
    }
#if IR_TX_EXT_SUPPORTED
    if(ir_tx.output_held) {
        if(ir_tx_output_active == FuriHalInfraredTxPinExtPA7) ir_tx_ext_release();
        ir_tx_pin_park_low((gpio_num_t)IR_TX_GPIO);
        ir_tx.output_held = false;
    }
#endif
}

static bool ir_tx_begin(uint32_t freq, float duty_cycle, bool carrier) {
    furi_check(ir_tx.data_callback != NULL);

    /* Take the peripheral: either the caller claimed it already, or it is
     * claimed here. Anything else means another user is transmitting or
     * receiving; refuse without touching its state. */
    bool owned = false;
    portENTER_CRITICAL(&ir_state_lock);
    if(ir_state == InfraredStateClaimedTx) {
        /* Somebody else's claim is not ours to start on: it would take over their
         * callbacks and their peripheral. Only the claiming task may start. */
        owned = (ir_tx.claim_owner == xTaskGetCurrentTaskHandle());
    } else if(ir_state == InfraredStateIdle) {
        ir_state = InfraredStateClaimedTx;
        ir_tx.claim_owner = xTaskGetCurrentTaskHandle();
        owned = true;
    }
    portEXIT_CRITICAL(&ir_state_lock);
    if(!owned) return false;

    ir_tx_last_error = FuriHalInfraredTxErrorNone;
    ir_tx.abort_requested = false;
    ir_tx.channel_disabled = true; /* not enabled yet */
    ir_tx.output_held = false;
    ir_tx.channel = NULL;
    ir_tx.copy_encoder = NULL;
    ir_tx.carrier_freq = freq;
    ir_tx.duty_cycle = duty_cycle;

    if(!BOARD_HAS_IR) {
        FURI_LOG_E("IR", "Board has no IR support");
        ir_tx_refuse(FuriHalInfraredTxErrorNoHardware);
        return true;
    }

    gpio_num_t tx_gpio = (gpio_num_t)IR_TX_GPIO;
#if IR_TX_EXT_SUPPORTED
    ir_tx_output_load();
    ir_tx_output_active = ir_tx_output_cfg;
    if(ir_tx_output_active == FuriHalInfraredTxPinExtPA7) {
        if(!ir_tx_ext_claim()) {
            ESP_LOGW(IR_EXT_TAG, "GPIO%d is in use; transmission refused", BOARD_PIN_IR_TX_EXT);
            ir_tx_refuse(FuriHalInfraredTxErrorExtPinBusy);
            return true;
        }
        /* Internal LED actively off while the external pin transmits. */
        ir_tx_pin_park_low((gpio_num_t)IR_TX_GPIO);
        tx_gpio = (gpio_num_t)BOARD_PIN_IR_TX_EXT;
    }
    /* Both outputs: the LED pad is parked low again when the transmission ends. */
    ir_tx.output_held = true;
#endif

    /* The channel is created directly on the selected pad: the RMT signal is
     * routed to exactly one pin, never to both. */
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
    rmt_carrier_config_t carrier_config = {
        .frequency_hz = freq,
        .duty_cycle = duty_cycle,
        .flags = {
            .polarity_active_low = false,
            .always_on = false, /* Only modulate during mark (level=1) */
        },
    };
    rmt_copy_encoder_config_t copy_enc_config = {};
    ir_tx.packet_done = xSemaphoreCreateBinary();
    esp_err_t err = ir_tx.packet_done ? ESP_OK : ESP_ERR_NO_MEM;
    if(err == ESP_OK) err = rmt_new_tx_channel(&tx_chan_config, &ir_tx.channel);
    if(err == ESP_OK) {
        const rmt_tx_event_callbacks_t tx_callbacks = {.on_trans_done = ir_rmt_tx_done_callback};
        err = rmt_tx_register_event_callbacks(ir_tx.channel, &tx_callbacks, NULL);
    }
    /* A plain pulse train has no carrier: the mark is the raw output level. */
    if(err == ESP_OK && carrier) err = rmt_apply_carrier(ir_tx.channel, &carrier_config);
    if(err == ESP_OK) err = rmt_new_copy_encoder(&copy_enc_config, &ir_tx.copy_encoder);
    if(err == ESP_OK) err = rmt_enable(ir_tx.channel);
    if(err == ESP_OK) ir_tx.channel_disabled = false;
    /* Synchronization semaphore (raw FreeRTOS, not Furi — tx task is xTaskCreate) */
    if(err == ESP_OK) {
        ir_tx.done_semaphore = xSemaphoreCreateBinary();
        if(!ir_tx.done_semaphore) err = ESP_ERR_NO_MEM;
    }
    if(err != ESP_OK) {
        FURI_LOG_E("IR", "RMT TX setup on GPIO%d failed: %s", (int)tx_gpio, esp_err_to_name(err));
        ir_tx_output_teardown();
        ir_tx_refuse(FuriHalInfraredTxErrorHardware);
        return true;
    }

    ir_state_set(InfraredStateAsyncTx);

    /* Start TX task */
    if(xTaskCreate(ir_tx_task, "ir_tx", 4096, NULL, 15, NULL) != pdPASS) {
        FURI_LOG_E("IR", "TX task creation failed");
        vSemaphoreDelete(ir_tx.done_semaphore);
        ir_tx.done_semaphore = NULL;
        ir_tx_output_teardown();
        ir_tx_refuse(FuriHalInfraredTxErrorHardware);
    }
    return true;
}

/* May the calling task change the TX callbacks right now? */
static bool ir_tx_may_configure(void) {
    bool ok;
    portENTER_CRITICAL(&ir_state_lock);
    ok = (ir_state == InfraredStateIdle) ||
         (ir_state == InfraredStateClaimedTx && ir_tx.claim_owner == xTaskGetCurrentTaskHandle());
    portEXIT_CRITICAL(&ir_state_lock);
    return ok;
}

bool furi_hal_infrared_claim_tx(void) {
    bool claimed = false;
    portENTER_CRITICAL(&ir_state_lock);
    if(ir_state == InfraredStateIdle) {
        ir_state = InfraredStateClaimedTx;
        ir_tx.claim_owner = xTaskGetCurrentTaskHandle();
        claimed = true;
    }
    portEXIT_CRITICAL(&ir_state_lock);
    if(!claimed) return false;
    /* Start from a clean slate: callbacks of an earlier user (a FAP that has
     * since been unloaded) must never be called for this transmission. */
    ir_tx.data_callback = NULL;
    ir_tx.data_context = NULL;
    ir_tx.signal_sent_callback = NULL;
    ir_tx.signal_sent_context = NULL;
    ir_tx.error_callback = NULL;
    ir_tx.error_context = NULL;
    return true;
}

void furi_hal_infrared_release_claim(void) {
    bool released = false;
    portENTER_CRITICAL(&ir_state_lock);
    if(ir_state == InfraredStateClaimedTx && ir_tx.claim_owner == xTaskGetCurrentTaskHandle()) {
        ir_state = InfraredStateIdle;
        ir_tx.claim_owner = NULL;
        released = true;
    }
    portEXIT_CRITICAL(&ir_state_lock);
    furi_check(released);
}

bool furi_hal_infrared_async_tx_start(uint32_t freq, float duty_cycle) {
    furi_check(freq >= INFRARED_MIN_FREQUENCY && freq <= INFRARED_MAX_FREQUENCY);
    furi_check(duty_cycle > 0.0f && duty_cycle <= 1.0f);
    return ir_tx_begin(freq, duty_cycle, true);
}

bool furi_hal_infrared_async_tx_start_plain(void) {
    return ir_tx_begin(0, 1.0f, false);
}

void furi_hal_infrared_async_tx_wait_termination(void) {
    furi_check(ir_state >= InfraredStateAsyncTx);
    furi_check(ir_state < InfraredStateMAX);

    /* Wait for TX task to finish */
    xSemaphoreTake(ir_tx.done_semaphore, portMAX_DELAY);

    /* A refused start has no channel and nothing to release. */
    ir_tx_output_teardown();

    vSemaphoreDelete(ir_tx.done_semaphore);
    ir_tx.done_semaphore = NULL;

    /* The error callback belongs to one transmission. The data and signal-sent
     * callbacks stay: the infrared worker starts several transmissions in a
     * row with the callbacks it registered once. */
    ir_tx.error_callback = NULL;
    ir_tx.error_context = NULL;
    ir_tx.abort_requested = false;
    ir_tx.claim_owner = NULL;

    ir_state_set(InfraredStateIdle);
}

void furi_hal_infrared_async_tx_stop(void) {
    furi_check(ir_state >= InfraredStateAsyncTx);
    furi_check(ir_state < InfraredStateMAX);

    /* Atomic: the TX task may just have moved to AsyncTxStopped. */
    ir_state_transition(InfraredStateAsyncTx, InfraredStateAsyncTxStopReq);

    furi_hal_infrared_async_tx_wait_termination();
}

void furi_hal_infrared_async_tx_abort(void) {
    furi_check(ir_state >= InfraredStateAsyncTx);
    furi_check(ir_state < InfraredStateMAX);

    ir_tx.abort_requested = true;
    ir_state_transition(InfraredStateAsyncTx, InfraredStateAsyncTxStopReq);

    furi_hal_infrared_async_tx_wait_termination();
}

void furi_hal_infrared_async_tx_set_data_isr_callback(
    FuriHalInfraredTxGetDataISRCallback callback,
    void* context) {
    /* Idle for the legacy "set, start" users, the owner's claim for the claim users.
     * Anything else would replace the callback of a transmission in flight, or of a
     * claimant that has not started yet. */
    furi_check(ir_tx_may_configure());
    ir_tx.data_callback = callback;
    ir_tx.data_context = context;
}

void furi_hal_infrared_async_tx_set_error_callback(
    FuriHalInfraredTxErrorCallback callback,
    void* context) {
    furi_check(ir_tx_may_configure());
    ir_tx.error_callback = callback;
    ir_tx.error_context = context;
}

void furi_hal_infrared_async_tx_set_signal_sent_isr_callback(
    FuriHalInfraredTxSignalSentISRCallback callback,
    void* context) {
    ir_tx.signal_sent_callback = callback;
    ir_tx.signal_sent_context = context;
}

bool furi_hal_infrared_async_tx_reset_callbacks(void) {
    bool done = false;
    portENTER_CRITICAL(&ir_state_lock);
    if(ir_state == InfraredStateIdle) {
        ir_tx.data_callback = NULL;
        ir_tx.data_context = NULL;
        ir_tx.signal_sent_callback = NULL;
        ir_tx.signal_sent_context = NULL;
        ir_tx.error_callback = NULL;
        ir_tx.error_context = NULL;
        done = true;
    }
    portEXIT_CRITICAL(&ir_state_lock);
    return done;
}

bool furi_hal_infrared_is_busy(void) {
    return ir_state != InfraredStateIdle;
}

bool furi_hal_infrared_is_available(void) {
    return BOARD_HAS_IR;
}

FuriHalInfraredTxPin furi_hal_infrared_detect_tx_output(void) {
    /* No module detection on ESP32 boards */
    return FuriHalInfraredTxPinInternal;
}

#if IR_TX_EXT_SUPPORTED
void furi_hal_infrared_set_tx_output(FuriHalInfraredTxPin tx_pin) {
    ir_tx_output_load();
    ir_tx_output_cfg = tx_pin == FuriHalInfraredTxPinExtPA7 ? FuriHalInfraredTxPinExtPA7 :
                                                             FuriHalInfraredTxPinInternal;
}

FuriHalInfraredTxPin furi_hal_infrared_get_tx_output(void) {
    ir_tx_output_load();
    return ir_tx_output_cfg;
}

bool furi_hal_infrared_is_tx_output_supported(FuriHalInfraredTxPin tx_pin) {
    return tx_pin == FuriHalInfraredTxPinInternal || tx_pin == FuriHalInfraredTxPinExtPA7;
}

uint16_t furi_hal_infrared_get_ext_tx_pin(void) {
    return BOARD_PIN_IR_TX_EXT;
}

void furi_hal_infrared_save_tx_output(FuriHalInfraredTxPin tx_pin) {
    furi_hal_infrared_set_tx_output(tx_pin);
    nvs_handle_t nvs;
    if(nvs_open(IR_TX_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, IR_TX_NVS_KEY, (uint8_t)ir_tx_output_cfg);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}
#else
void furi_hal_infrared_set_tx_output(FuriHalInfraredTxPin tx_pin) {
    /* Only internal pin supported, ignore */
    (void)tx_pin;
}

FuriHalInfraredTxPin furi_hal_infrared_get_tx_output(void) {
    return FuriHalInfraredTxPinInternal;
}

bool furi_hal_infrared_is_tx_output_supported(FuriHalInfraredTxPin tx_pin) {
    return tx_pin == FuriHalInfraredTxPinInternal;
}

uint16_t furi_hal_infrared_get_ext_tx_pin(void) {
    return UINT16_MAX;
}

void furi_hal_infrared_save_tx_output(FuriHalInfraredTxPin tx_pin) {
    (void)tx_pin;
}
#endif

FuriHalInfraredTxError furi_hal_infrared_get_last_tx_error(void) {
    return ir_tx_last_error;
}
