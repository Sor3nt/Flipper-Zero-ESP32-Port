#include "wifi_marauder_app_i.h"
#include "wifi_marauder_uart.h"

#include <driver/gpio.h>
#include <driver/uart.h>
#include <furi_hal_external_uart.h>
#include <furi_hal_shared_pins.h>
#include <furi_hal_uart_shared.h>

// ESP32 port notes (details: PORT_NOTES.md)
//  - This firmware has no furi_hal_serial. The external UART header (GPIO43/44) is shared by the
//    USB-UART bridge, BW16, the GPS wardriver, the background RFID reader and,
//    on T-Embed, the NRF24 control lines (CE/CSN). The app therefore takes it exactly the way
//    those do, in this order, and gives it back in reverse:
//        1. furi_hal_uart_shared_acquire  serializes UART users and preempts the RFID reader
//        2. furi_hal_shared_pins_acquire  cooperative lease on the GPIO43/44 pads
//        3. driver check                  never touch a UART driver somebody else installed
//        4. furi_hal_shared_pins_save     snapshot of the pads, restored at the end
//        5. light-sleep veto              light sleep gates the UART clock and drops bytes
//        6. UART driver on the board's UART number, pins from the HAL
//    Every step records what it owns; a cleanup that fails keeps what is still owned and is
//    retried instead of being forgotten. Release runs the same order the other UART users
//    (USB-UART bridge, BW16, GPS) use: driver, pad snapshot, UART lease, pin lease.
//  - The UART number and the pads come from furi_hal_usb_uart_bridge_get_pins(): one binary then
//    works on every board, and it answers "no" on boards without a UART header (ThorChimera,
//    Thormini) and on T-Embed unless the external-UART profile is built in (the pads are the
//    NRF24 CE/CSN lines there).
//  - Receiving is a thread that polls uart_read_bytes() and runs the [BUF/BEGIN] / [BUF/CLOSE]
//    PCAP marker parser inline; the data callbacks run in that thread, as they did in the
//    upstream worker thread.

#define BAUDRATE 115200
#define UART_DRIVER_RX_BUFFER 4096
#define UART_LEASE_MS 1000
#define UART_PINS_TRIES 40
#define UART_PINS_TRY_MS 5
#define UART_READ_TIMEOUT_MS 20
#define UART_CLOSE_TRIES 5
#define UART_CLOSE_RETRY_MS 100
#define MARKER_LEN 11

static const char wifi_marauder_pins_owner[] = "WiFi Marauder";
static const char* wifi_marauder_uart_error = "UART not started";

struct WifiMarauderUart {
    WifiMarauderApp* app;
    FuriThread* rx_thread;
    volatile bool stop;

    // What this app owns (see the order above).
    bool uart_leased;
    bool pins_leased;
    bool pins_saved;
    bool sleep_inhibited;
    bool driver;
    int uart_num;
    int tx_pin;
    int rx_pin;

    // [BUF/BEGIN] .. [BUF/CLOSE] marker parser state
    bool pcap;
    uint8_t mark_test_buf[MARKER_LEN];
    uint8_t mark_test_idx;

    // Staging buffer of the receiver thread (not on its stack). One fed byte adds at most
    // MARKER_LEN bytes (a flushed partial marker plus the byte), and the buffer is flushed
    // once it holds RX_BUF_SIZE, so it never overflows.
    uint8_t rx_buf[RX_BUF_SIZE + MARKER_LEN + 1];
    // Held while a data callback runs, and by the setters below: when a setter returns, the
    // callback it replaced is not running (the scene that owned it may then free its state).
    FuriMutex* cb_mutex;
    void (*handle_rx_data_cb)(uint8_t* buf, size_t len, void* context);
    void (*handle_rx_pcap_cb)(uint8_t* buf, size_t len, void* context);
};

const char* wifi_marauder_uart_last_error(void) {
    return wifi_marauder_uart_error;
}

void wifi_marauder_uart_set_handle_rx_data_cb(
    WifiMarauderUart* uart,
    void (*handle_rx_data_cb)(uint8_t* buf, size_t len, void* context)) {
    furi_assert(uart);
    furi_check(furi_mutex_acquire(uart->cb_mutex, FuriWaitForever) == FuriStatusOk);
    uart->handle_rx_data_cb = handle_rx_data_cb;
    furi_check(furi_mutex_release(uart->cb_mutex) == FuriStatusOk);
}

void wifi_marauder_uart_set_handle_rx_pcap_cb(
    WifiMarauderUart* uart,
    void (*handle_rx_pcap_cb)(uint8_t* buf, size_t len, void* context)) {
    furi_assert(uart);
    furi_check(furi_mutex_acquire(uart->cb_mutex, FuriWaitForever) == FuriStatusOk);
    uart->handle_rx_pcap_cb = handle_rx_pcap_cb;
    furi_check(furi_mutex_release(uart->cb_mutex) == FuriStatusOk);
}

// Hand a run of received bytes to the console or the PCAP sink.
static void wifi_marauder_uart_deliver(WifiMarauderUart* uart, bool pcap, uint8_t* data, size_t len) {
    if(len == 0) return;
    furi_check(furi_mutex_acquire(uart->cb_mutex, FuriWaitForever) == FuriStatusOk);
    if(pcap) {
        if(uart->handle_rx_pcap_cb) uart->handle_rx_pcap_cb(data, len, uart->app);
    } else {
        if(uart->handle_rx_data_cb) uart->handle_rx_data_cb(data, len, uart->app);
    }
    furi_check(furi_mutex_release(uart->cb_mutex) == FuriStatusOk);
}

// Same state machine as upstream, byte by byte. `out`/`out_len` collect the bytes of the current
// mode; a mode switch (a complete marker) flushes them first.
static void wifi_marauder_uart_feed(
    WifiMarauderUart* uart,
    uint8_t data,
    uint8_t* out,
    size_t* out_len,
    bool* out_pcap) {
    static const char mark_begin[] = "[BUF/BEGIN]";
    static const char mark_close[] = "[BUF/CLOSE]";

    if(uart->mark_test_idx != 0) {
        // Trying to match a marker.
        if(data == (uint8_t)mark_begin[uart->mark_test_idx] ||
           data == (uint8_t)mark_close[uart->mark_test_idx]) {
            uart->mark_test_buf[uart->mark_test_idx++] = data;
            if(uart->mark_test_idx == MARKER_LEN) {
                const bool begin = !memcmp(uart->mark_test_buf, mark_begin, MARKER_LEN);
                const bool close = !memcmp(uart->mark_test_buf, mark_close, MARKER_LEN);
                if(begin || close) {
                    // The bytes before the marker belong to the old mode.
                    wifi_marauder_uart_deliver(uart, *out_pcap, out, *out_len);
                    *out_len = 0;
                    uart->pcap = begin;
                    *out_pcap = uart->pcap;
                } else {
                    // Eleven bytes that matched position by position but are neither marker
                    // ("[BUF/BLOSE]"): ordinary data, not to be swallowed.
                    for(uint8_t i = 0; i < MARKER_LEN; i++) out[(*out_len)++] = uart->mark_test_buf[i];
                }
                uart->mark_test_idx = 0;
            }
            return; // part of a marker: not data
        }
        // Not a marker after all: the partial match was ordinary data.
        for(uint8_t i = 0; i < uart->mark_test_idx; i++) {
            out[(*out_len)++] = uart->mark_test_buf[i];
        }
        uart->mark_test_idx = 0;
    }

    if(data == (uint8_t)mark_begin[0]) {
        uart->mark_test_buf[uart->mark_test_idx++] = data;
    } else {
        out[(*out_len)++] = data;
    }
}

static int32_t uart_worker(void* context) {
    WifiMarauderUart* uart = context;
    uint8_t chunk[256];
    uint8_t* out = uart->rx_buf; // not on this thread's stack: the data callbacks run on it

    while(!uart->stop) {
        const int received = uart_read_bytes(
            (uart_port_t)uart->uart_num, chunk, sizeof(chunk), pdMS_TO_TICKS(UART_READ_TIMEOUT_MS));
        if(received < 0) break; // the driver is gone
        if(received == 0) continue;

        size_t out_len = 0;
        bool out_pcap = uart->pcap;
        for(int i = 0; i < received; i++) {
            wifi_marauder_uart_feed(uart, chunk[i], out, &out_len, &out_pcap);
            if(out_len >= RX_BUF_SIZE) {
                wifi_marauder_uart_deliver(uart, out_pcap, out, out_len);
                out_len = 0;
            }
        }
        wifi_marauder_uart_deliver(uart, out_pcap, out, out_len);
    }
    return 0;
}

void wifi_marauder_uart_tx(WifiMarauderUart* uart, uint8_t* data, size_t len) {
    if(!uart || !uart->driver || !data || len == 0) return;
    uart_write_bytes((uart_port_t)uart->uart_num, (const char*)data, len);
}

// Leave TX and RX as inputs with a pull-up (a floating pad would read noise as data).
static esp_err_t wifi_marauder_uart_park_pins(const WifiMarauderUart* uart) {
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << uart->tx_pin) | (1ULL << uart->rx_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

// Give back, in reverse order, whatever is still owned. True only when nothing is. Safe to call
// again after a failure: it continues where it stopped.
static bool wifi_marauder_uart_release(WifiMarauderUart* uart) {
    if(uart->driver) {
        // Flush what we still send, then drop the driver. A driver that is still installed is
        // still ours: keep every lease.
        uart_wait_tx_done((uart_port_t)uart->uart_num, pdMS_TO_TICKS(250));
        const esp_err_t err = uart_driver_delete((uart_port_t)uart->uart_num);
        if(err != ESP_OK && uart_is_driver_installed((uart_port_t)uart->uart_num)) return false;
        uart->driver = false;
        wifi_marauder_uart_park_pins(uart);
    }
    // Put the pads back exactly as found (T-Embed: the NRF24 CE/CSN state), then drop the
    // reservation uart_set_pin() made. The pin lease cannot be released while this is saved.
    if(uart->pins_saved) {
        furi_hal_shared_pins_restore(wifi_marauder_pins_owner);
        if(furi_hal_shared_pins_is_saved(wifi_marauder_pins_owner)) return false;
        uart->pins_saved = false;
    }
    if(uart->uart_leased) {
        if(!furi_hal_uart_shared_release()) return false;
        uart->uart_leased = false;
    }
    if(uart->pins_leased) {
        furi_hal_shared_pins_release(wifi_marauder_pins_owner);
        uart->pins_leased = false;
    }
    // Last: while the driver or a lease is still ours the UART may need the clock.
    if(uart->sleep_inhibited) {
        furi_hal_power_light_sleep_inhibit_exit();
        uart->sleep_inhibited = false;
    }
    return !uart->driver && !uart->pins_saved && !uart->uart_leased && !uart->pins_leased &&
           !uart->sleep_inhibited;
}

static bool wifi_marauder_uart_acquire(WifiMarauderUart* uart) {
    int uart_num = 0, tx = 0, rx = 0;
    if(!furi_hal_usb_uart_bridge_get_pins(&uart_num, &tx, &rx)) {
        wifi_marauder_uart_error = "No UART header on\nthis board";
        return false;
    }
    uart->uart_num = uart_num;
    uart->tx_pin = tx;
    uart->rx_pin = rx;

    // 1) every shared UART user goes through this lease (and the RFID reader yields to it)
    const FuriHalUartSharedAcquireResult lease = furi_hal_uart_shared_acquire(UART_LEASE_MS);
    if(lease != FuriHalUartSharedAcquireAcquired) {
        wifi_marauder_uart_error = lease == FuriHalUartSharedAcquireBusy ?
                                       "UART busy: close\nother UART apps" :
                                       "UART lease failed";
        return false;
    }
    uart->uart_leased = true;

    // 2) the GPIO43/44 pads (NRF24 / IR / RFID / BW16 share them)
    bool pins = false;
    for(unsigned i = 0; i < UART_PINS_TRIES && !pins; i++) {
        pins = furi_hal_shared_pins_acquire(wifi_marauder_pins_owner);
        if(!pins) furi_delay_ms(UART_PINS_TRY_MS);
    }
    if(!pins) {
        wifi_marauder_uart_error = "Pins busy: close\nNRF24 / IR apps";
        return false;
    }
    uart->pins_leased = true;

    // 3) older apps install UART1 / UART2 without any lease: never reconfigure a driver this app
    // did not install. Checked before the pads are touched, so a refusal leaves them alone.
    if(uart_is_driver_installed((uart_port_t)uart_num) || uart_is_driver_installed(UART_NUM_1) ||
       uart_is_driver_installed(UART_NUM_2)) {
        wifi_marauder_uart_error = "UART busy: close\nGPS / BW16 apps";
        return false;
    }

    // 4) snapshot of the pads (false: this board has nothing to snapshot)
    if(furi_hal_shared_pins_save(wifi_marauder_pins_owner)) uart->pins_saved = true;

    // 5) light sleep would gate the UART clock and drop bytes
    furi_hal_power_light_sleep_inhibit_enter();
    uart->sleep_inhibited = true;

    // 6) the driver
    if(uart_driver_install((uart_port_t)uart_num, UART_DRIVER_RX_BUFFER, 0, 0, NULL, 0) != ESP_OK) {
        wifi_marauder_uart_error = "UART driver install\nfailed";
        return false;
    }
    uart->driver = true;

    const uart_config_t config = {
        .baud_rate = BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config((uart_port_t)uart_num, &config);
    if(err == ESP_OK) err = wifi_marauder_uart_park_pins(uart);
    if(err == ESP_OK) {
        err = uart_set_pin(
            (uart_port_t)uart_num, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if(err == ESP_OK) err = gpio_set_pull_mode((gpio_num_t)rx, GPIO_PULLUP_ONLY);
    if(err == ESP_OK) err = uart_flush_input((uart_port_t)uart_num);
    if(err != ESP_OK) {
        wifi_marauder_uart_error = "UART pin setup failed";
        return false;
    }
    return true;
}

WifiMarauderUart* wifi_marauder_usart_init(WifiMarauderApp* app) {
    wifi_marauder_uart_error = "Out of memory";
    WifiMarauderUart* uart = calloc(1, sizeof(WifiMarauderUart));
    if(!uart) return NULL;
    uart->app = app;
    uart->cb_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!uart->cb_mutex) {
        free(uart);
        return NULL;
    }

    if(!wifi_marauder_uart_acquire(uart)) goto fail;

    wifi_marauder_uart_error = "Out of memory";
    // The data callbacks (text box, files, string growth) run on this thread's stack.
    uart->rx_thread = furi_thread_alloc_ex("WifiMarauderUartRx", 6144, uart_worker, uart);
    if(!uart->rx_thread) goto fail;
    furi_thread_start(uart->rx_thread);
    wifi_marauder_uart_error = "";
    return uart;

fail:
    // The retry in wifi_marauder_uart_free() is not available here: the half-started session is
    // simply released; whatever cannot be released stays reserved and is logged.
    for(uint8_t attempt = 0; attempt < UART_CLOSE_TRIES; attempt++) {
        if(wifi_marauder_uart_release(uart)) break;
        furi_delay_ms(UART_CLOSE_RETRY_MS);
        if(attempt + 1 == UART_CLOSE_TRIES) {
            FURI_LOG_E("WifiMarauder", "UART header could not be released; it stays reserved until reboot");
        }
    }
    furi_mutex_free(uart->cb_mutex);
    free(uart);
    return NULL;
}

void wifi_marauder_uart_free(WifiMarauderUart* uart) {
    // The app calls this also when the UART could not be started (nothing to free).
    if(!uart) return;

    // The receiver first: it reads from the driver that is about to be removed.
    uart->stop = true;
    if(uart->rx_thread) {
        furi_thread_join(uart->rx_thread);
        furi_thread_free(uart->rx_thread);
        uart->rx_thread = NULL;
    }

    bool released = false;
    for(uint8_t attempt = 0; attempt < UART_CLOSE_TRIES && !released; attempt++) {
        released = wifi_marauder_uart_release(uart);
        if(!released) furi_delay_ms(UART_CLOSE_RETRY_MS);
    }
    if(!released) {
        // The FAP is unloaded after this returns: a lease still held cannot be given back.
        FURI_LOG_E("WifiMarauder", "UART header could not be released; it stays reserved until reboot");
    }
    furi_mutex_free(uart->cb_mutex);
    free(uart);
}
