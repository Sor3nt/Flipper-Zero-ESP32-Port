/**
 * RFID HAL for ESP32 — RDM6300 UART bridge
 *
 * The RDM6300 is a 125 kHz read-only module that decodes EM4100 tags
 * internally and outputs the result over UART (9600 8N1).
 *
 * Packet format (14 bytes):
 *   0x02  | 10 hex-ASCII chars (ver 2 + data 8) | 2 hex-ASCII checksum | 0x03
 *
 * This HAL reconstructs the EM4100 Manchester-encoded bit stream from the
 * decoded UART data and feeds it as (level, duration) pairs to the Flipper
 * protocol decoders via the capture callback, so the existing lfrfid worker
 * and protocol stack work unmodified.
 */

#include "furi_hal_rfid.h"
#include "furi_hal_shared_pins.h"
#include "furi_hal_uart_shared.h"
#include <furi.h>
#include <esp_log.h>
#include <stdatomic.h>

#include BOARD_INCLUDE

#define TAG "FuriHalRfid"

#if BOARD_HAS_RFID

#include <driver/uart.h>

/* ---- RDM6300 protocol constants ---- */
#define RDM_BAUD        9600
#define RDM_PKT_SIZE    14
#define RDM_START       0x02
#define RDM_END         0x03
#define RDM_BUF_SIZE    256

/* ---- EM4100 timing (RF/64 at 125 kHz) ---- */
#define EM_BIT_PERIOD_US 512   /* one full bit = 512 µs */
#define EM_HALF_BIT_US   256   /* Manchester half-bit */

/* ---- State ---- */
static FuriHalRfidReadCaptureCallback s_capture_cb = NULL;
static void* s_capture_ctx = NULL;
typedef enum {
    RfidReaderStateIdle,
    RfidReaderStateStarting,
    RfidReaderStateRunning,
    RfidReaderStateStopping,
} RfidReaderState;

static atomic_int s_reader_state = ATOMIC_VAR_INIT(RfidReaderStateIdle);
static FuriThread* s_reader_thread = NULL;

static bool rfid_reader_is_running(void) {
    return atomic_load_explicit(&s_reader_state, memory_order_acquire) == RfidReaderStateRunning;
}

/* ---- Helpers ---- */

static uint8_t hex_char_to_nibble(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0xFF;
}

static bool rdm_parse_packet(const uint8_t* pkt, uint8_t* out_data, size_t out_len) {
    /* pkt[0]=0x02, pkt[1..10]=hex, pkt[11..12]=checksum hex, pkt[13]=0x03 */
    if(pkt[0] != RDM_START || pkt[13] != RDM_END) return false;
    if(out_len < 5) return false;

    uint8_t check = 0;
    for(int i = 0; i < 5; i++) {
        uint8_t hi = hex_char_to_nibble(pkt[1 + i * 2]);
        uint8_t lo = hex_char_to_nibble(pkt[2 + i * 2]);
        if(hi == 0xFF || lo == 0xFF) return false;
        out_data[i] = (hi << 4) | lo;
        check ^= out_data[i];
    }

    uint8_t chi = hex_char_to_nibble(pkt[11]);
    uint8_t clo = hex_char_to_nibble(pkt[12]);
    if(chi == 0xFF || clo == 0xFF) return false;
    uint8_t pkt_check = (chi << 4) | clo;

    return check == pkt_check;
}

/**
 * Build the 64-bit EM4100 frame from 5 decoded data bytes.
 *
 * Layout: 9 header bits (1) | 10 rows of (4 data + 1 parity) | 4 col parity | 0 stop
 */
static void em4100_build_frame(const uint8_t* data, uint8_t* frame_bits, size_t* bit_count) {
    size_t pos = 0;

    /* 9 header bits = 1 */
    for(int i = 0; i < 9; i++) frame_bits[pos++] = 1;

    /* Expand 5 bytes → 10 nibbles, each row = 4 data bits + even parity */
    uint8_t nibbles[10];
    for(int i = 0; i < 5; i++) {
        nibbles[i * 2 + 0] = (data[i] >> 4) & 0x0F;
        nibbles[i * 2 + 1] = data[i] & 0x0F;
    }

    uint8_t col_parity[4] = {0, 0, 0, 0};
    for(int row = 0; row < 10; row++) {
        uint8_t row_parity = 0;
        for(int col = 0; col < 4; col++) {
            uint8_t bit = (nibbles[row] >> (3 - col)) & 1;
            frame_bits[pos++] = bit;
            row_parity ^= bit;
            col_parity[col] ^= bit;
        }
        frame_bits[pos++] = row_parity;
    }

    /* 4 column parity bits */
    for(int col = 0; col < 4; col++) {
        frame_bits[pos++] = col_parity[col];
    }

    /* Stop bit = 0 */
    frame_bits[pos++] = 0;

    *bit_count = pos; /* should be 64 */
}

/**
 * Feed one EM4100 frame worth of Manchester-encoded edges to the capture
 * callback, mimicking what a real comparator + timer would produce.
 *
 * Manchester convention (as Flipper decoders expect):
 *   bit 1 → first half HIGH, second half LOW
 *   bit 0 → first half LOW,  second half HIGH
 *
 * We merge consecutive same-level half-bits to produce realistic edge
 * timing (short = 256 µs, long = 512 µs).
 */
static void em4100_feed_manchester(
    const uint8_t* frame_bits,
    size_t bit_count,
    FuriHalRfidReadCaptureCallback cb,
    void* ctx) {
    if(!cb) return;

    bool current_level = true; /* start high (header bit 1 → HIGH first) */
    uint32_t accum_us = 0;

    for(size_t i = 0; i < bit_count && rfid_reader_is_running(); i++) {
        /* Each bit has two half-bit phases */
        bool first_half = (frame_bits[i] == 1); /* 1→HIGH first, 0→LOW first */
        bool second_half = !first_half;

        /* First half-bit */
        if(first_half == current_level) {
            accum_us += EM_HALF_BIT_US;
        } else {
            cb(current_level, accum_us, ctx);
            current_level = first_half;
            accum_us = EM_HALF_BIT_US;
        }

        /* Second half-bit */
        if(second_half == current_level) {
            accum_us += EM_HALF_BIT_US;
        } else {
            cb(current_level, accum_us, ctx);
            current_level = second_half;
            accum_us = EM_HALF_BIT_US;
        }
    }

    /* Flush remaining */
    if(accum_us > 0 && rfid_reader_is_running()) {
        cb(current_level, accum_us, ctx);
    }
}

/* ---- Reader thread ---- */

typedef enum {
    RfidSessionDone,
    RfidSessionYielded,
    RfidSessionRetry,
    RfidSessionFailed,
} RfidSessionResult;

static RfidSessionResult rfid_reader_run_session(void) {
    bool shared_pins_acquired = false;
    bool shared_pins_saved = false;
    bool shared_uart_acquired = false;
    bool installed = false;
    bool yielded = false;
    bool retry = false;
    esp_err_t uart_err = ESP_OK;

    if(!rfid_reader_is_running()) return RfidSessionDone;

    if(furi_hal_uart_shared_acquire_background(100) !=
       FuriHalUartSharedAcquireAcquired) {
        return rfid_reader_is_running() ? RfidSessionRetry : RfidSessionDone;
    }
    shared_uart_acquired = true;

    if(!furi_hal_shared_pins_acquire(&s_reader_thread)) {
        FURI_LOG_W(TAG, "RFID shared pins are busy; retrying after releasing UART");
        retry = true;
        goto release;
    }
    shared_pins_acquired = true;
    if(!rfid_reader_is_running()) goto release;
    if(uart_is_driver_installed(BOARD_RFID_UART_NUM)) {
        FURI_LOG_E(TAG, "RFID UART driver is already installed");
        goto failed;
    }

#if defined(BOARD_EXTERNAL_UART_SAVE_RESTORE_SHARED_PINS)
    if(!furi_hal_shared_pins_save(&s_reader_thread)) {
        FURI_LOG_E(TAG, "Could not preserve shared RFID pin state");
        goto failed;
    }
    shared_pins_saved = true;
#endif

    uart_config_t uart_cfg = {
        .baud_rate = RDM_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* Install before touching configuration. Never reconfigure/delete an owner. */
    uart_err = uart_driver_install(BOARD_RFID_UART_NUM, RDM_BUF_SIZE, 0, 0, NULL, 0);
    if(uart_err != ESP_OK) {
        FURI_LOG_E(TAG, "RFID UART driver install failed: %s", esp_err_to_name(uart_err));
        goto failed;
    }
    installed = true;
    uart_err = uart_param_config(BOARD_RFID_UART_NUM, &uart_cfg);
    if(uart_err == ESP_OK) {
        uart_err = uart_set_pin(
            BOARD_RFID_UART_NUM, BOARD_PIN_RFID_TX, BOARD_PIN_RFID_RX, -1, -1);
    }
    if(uart_err != ESP_OK) {
        FURI_LOG_E(TAG, "RFID UART setup failed: %s", esp_err_to_name(uart_err));
        goto release;
    }

    uint8_t byte_buf[RDM_PKT_SIZE];
    size_t pkt_pos = 0;

    while(rfid_reader_is_running()) {
        if(furi_hal_uart_shared_preemption_requested()) {
            yielded = true;
            break;
        }
        uint8_t c;
        const int len = uart_read_bytes(BOARD_RFID_UART_NUM, &c, 1, pdMS_TO_TICKS(50));
        if(len < 0) {
            FURI_LOG_E(TAG, "RFID UART read failed");
            goto release;
        }
        if(len == 0) continue;

        if(c == RDM_START) {
            byte_buf[0] = c;
            pkt_pos = 1;
            continue;
        }

        if(pkt_pos == 0) continue; /* waiting for start */

        byte_buf[pkt_pos++] = c;

        if(pkt_pos < RDM_PKT_SIZE) continue;

        /* Full packet received */
        pkt_pos = 0;

        uint8_t card_data[5];
        if(!rdm_parse_packet(byte_buf, card_data, sizeof(card_data))) {
            ESP_LOGW(TAG, "RDM6300 checksum mismatch");
            continue;
        }

        ESP_LOGI(TAG, "RDM6300 tag: %02X%02X%02X%02X%02X",
                 card_data[0], card_data[1], card_data[2], card_data[3], card_data[4]);

        /* Build EM4100 64-bit frame and feed as Manchester to decoder */
        uint8_t frame_bits[64];
        size_t bit_count = 0;
        em4100_build_frame(card_data, frame_bits, &bit_count);

        /* Feed the frame multiple times (real cards repeat continuously) */
        for(int rep = 0; rep < 8 && rfid_reader_is_running(); rep++) {
            em4100_feed_manchester(frame_bits, bit_count, s_capture_cb, s_capture_ctx);
        }
    }

release:
    if(installed) {
        uart_err = uart_driver_delete(BOARD_RFID_UART_NUM);
        if(uart_err != ESP_OK && uart_is_driver_installed(BOARD_RFID_UART_NUM)) {
            FURI_LOG_E(
                TAG,
                "RFID UART driver removal failed; retaining shared leases: %s",
                esp_err_to_name(uart_err));
            return RfidSessionFailed;
        }
        if(uart_err != ESP_OK) FURI_LOG_W(TAG, "RFID UART driver was already removed");
    }
    if(shared_pins_saved) {
        furi_hal_shared_pins_restore(&s_reader_thread);
        if(furi_hal_shared_pins_is_saved(&s_reader_thread)) {
            FURI_LOG_E(TAG, "RFID UART pin restore failed; retaining shared leases");
            return RfidSessionFailed;
        }
        shared_pins_saved = false;
    }
    if(shared_uart_acquired) {
        if(!furi_hal_uart_shared_release()) {
            FURI_LOG_E(TAG, "Failed to release shared RFID UART; retaining pin lease");
            return RfidSessionFailed;
        }
        shared_uart_acquired = false;
    }
    if(shared_pins_acquired) {
        furi_hal_shared_pins_release(&s_reader_thread);
        shared_pins_acquired = false;
    }

    if(yielded) return RfidSessionYielded;
    if(retry) return RfidSessionRetry;
    return RfidSessionDone;

failed:
    if(installed) {
        uart_err = uart_driver_delete(BOARD_RFID_UART_NUM);
        if(uart_err != ESP_OK && uart_is_driver_installed(BOARD_RFID_UART_NUM)) {
            FURI_LOG_E(TAG, "RFID UART setup cleanup failed; retaining shared leases");
            return RfidSessionFailed;
        }
        installed = false;
    }
    if(shared_pins_saved) {
        furi_hal_shared_pins_restore(&s_reader_thread);
        if(furi_hal_shared_pins_is_saved(&s_reader_thread)) {
            FURI_LOG_E(TAG, "RFID UART pin restore failed; retaining shared leases");
            return RfidSessionFailed;
        }
        shared_pins_saved = false;
    }
    if(shared_uart_acquired && !furi_hal_uart_shared_release()) {
        FURI_LOG_E(TAG, "RFID UART setup could not release shared UART");
        return RfidSessionFailed;
    }
    if(shared_pins_acquired) furi_hal_shared_pins_release(&s_reader_thread);
    return RfidSessionFailed;
}

static int32_t rfid_reader_thread(void* arg) {
    UNUSED(arg);
    while(atomic_load_explicit(&s_reader_state, memory_order_acquire) ==
          RfidReaderStateStarting) {
        furi_delay_ms(1);
    }
    if(!rfid_reader_is_running()) return 0;

    ESP_LOGI(TAG, "RFID reader thread started (UART%d, RX=%d TX=%d)",
             BOARD_RFID_UART_NUM, BOARD_PIN_RFID_RX, BOARD_PIN_RFID_TX);
    while(rfid_reader_is_running()) {
        const RfidSessionResult result = rfid_reader_run_session();
        if(result == RfidSessionYielded || result == RfidSessionRetry) {
            furi_delay_ms(10);
            continue;
        }
        break;
    }

    /* Keep the state non-idle until stop joins/frees this thread, even after failure. */
    ESP_LOGI(TAG, "RFID UART session ended (busy/setup error or stop)");
    ESP_LOGI(TAG, "RFID reader thread stopped");
    return 0;
}

/* ---- HAL API ---- */

void furi_hal_rfid_init(void) {
    FURI_LOG_I(TAG, "RFID HAL initialized (RDM6300 UART bridge)");
}

void furi_hal_rfid_pins_reset(void) {
}

void furi_hal_rfid_pin_pull_release(void) {
}

void furi_hal_rfid_pin_pull_pulldown(void) {
}

void furi_hal_rfid_tim_read_start(float freq, float duty_cycle) {
    (void)freq;
    (void)duty_cycle;
    /* RDM6300 generates its own 125 kHz field — nothing to do */
}

void furi_hal_rfid_tim_read_pause(void) {
}

void furi_hal_rfid_tim_read_continue(void) {
}

void furi_hal_rfid_tim_read_stop(void) {
}

void furi_hal_rfid_tim_read_capture_start(
    FuriHalRfidReadCaptureCallback callback,
    void* context) {
    int expected = RfidReaderStateIdle;
    if(!atomic_compare_exchange_strong_explicit(
           &s_reader_state,
           &expected,
           RfidReaderStateStarting,
           memory_order_acq_rel,
           memory_order_acquire)) {
        return;
    }

    s_capture_cb = callback;
    s_capture_ctx = context;

    s_reader_thread = furi_thread_alloc();
    if(!s_reader_thread) {
        FURI_LOG_E(TAG, "RFID reader thread allocation failed");
        s_capture_cb = NULL;
        s_capture_ctx = NULL;
        atomic_store_explicit(&s_reader_state, RfidReaderStateIdle, memory_order_release);
        return;
    }
    furi_thread_set_name(s_reader_thread, "RfidRdm");
    furi_thread_set_stack_size(s_reader_thread, 4096);
    furi_thread_set_callback(s_reader_thread, rfid_reader_thread);
    furi_thread_start(s_reader_thread);
    atomic_store_explicit(&s_reader_state, RfidReaderStateRunning, memory_order_release);
}

void furi_hal_rfid_tim_read_capture_stop(void) {
    int expected;
    while(true) {
        expected = atomic_load_explicit(&s_reader_state, memory_order_acquire);
        if(expected == RfidReaderStateIdle) return;
        if(expected == RfidReaderStateStarting || expected == RfidReaderStateStopping) {
            furi_delay_ms(1);
            continue;
        }
        if(atomic_compare_exchange_weak_explicit(
               &s_reader_state,
               &expected,
               RfidReaderStateStopping,
               memory_order_acq_rel,
               memory_order_acquire)) {
            break;
        }
    }
    if(s_reader_thread) {
        furi_thread_join(s_reader_thread);
        furi_thread_free(s_reader_thread);
        s_reader_thread = NULL;
    }
    s_capture_cb = NULL;
    s_capture_ctx = NULL;
    atomic_store_explicit(&s_reader_state, RfidReaderStateIdle, memory_order_release);
}

void furi_hal_rfid_tim_emulate_dma_start(
    uint32_t* duration,
    uint32_t* pulse,
    size_t length,
    FuriHalRfidDMACallback callback,
    void* context) {
    (void)duration;
    (void)pulse;
    (void)length;
    (void)callback;
    (void)context;
    FURI_LOG_W(TAG, "RFID emulation not supported (RDM6300 is read-only)");
}

void furi_hal_rfid_tim_emulate_dma_stop(void) {
}

void furi_hal_rfid_set_read_period(uint32_t period) {
    (void)period;
}

void furi_hal_rfid_set_read_pulse(uint32_t pulse) {
    (void)pulse;
}

void furi_hal_rfid_comp_start(void) {
}

void furi_hal_rfid_comp_stop(void) {
}

void furi_hal_rfid_comp_set_callback(FuriHalRfidCompCallback callback, void* context) {
    (void)callback;
    (void)context;
}

void furi_hal_rfid_field_detect_start(void) {
}

void furi_hal_rfid_field_detect_stop(void) {
}

bool furi_hal_rfid_field_is_present(uint32_t* frequency) {
    (void)frequency;
    return false;
}

#else /* !BOARD_HAS_RFID */

void furi_hal_rfid_init(void) {
    FURI_LOG_I(TAG, "RFID not available on this board");
}

void furi_hal_rfid_pins_reset(void) { }
void furi_hal_rfid_pin_pull_release(void) { }
void furi_hal_rfid_pin_pull_pulldown(void) { }
void furi_hal_rfid_tim_read_start(float freq, float duty_cycle) { (void)freq; (void)duty_cycle; }
void furi_hal_rfid_tim_read_pause(void) { }
void furi_hal_rfid_tim_read_continue(void) { }
void furi_hal_rfid_tim_read_stop(void) { }
void furi_hal_rfid_tim_read_capture_start(FuriHalRfidReadCaptureCallback cb, void* ctx) { (void)cb; (void)ctx; }
void furi_hal_rfid_tim_read_capture_stop(void) { }
void furi_hal_rfid_tim_emulate_dma_start(uint32_t* d, uint32_t* p, size_t l, FuriHalRfidDMACallback cb, void* ctx) { (void)d; (void)p; (void)l; (void)cb; (void)ctx; }
void furi_hal_rfid_tim_emulate_dma_stop(void) { }
void furi_hal_rfid_set_read_period(uint32_t p) { (void)p; }
void furi_hal_rfid_set_read_pulse(uint32_t p) { (void)p; }
void furi_hal_rfid_comp_start(void) { }
void furi_hal_rfid_comp_stop(void) { }
void furi_hal_rfid_comp_set_callback(FuriHalRfidCompCallback cb, void* ctx) { (void)cb; (void)ctx; }
void furi_hal_rfid_field_detect_start(void) { }
void furi_hal_rfid_field_detect_stop(void) { }
bool furi_hal_rfid_field_is_present(uint32_t* f) { (void)f; return false; }

#endif /* BOARD_HAS_RFID */
