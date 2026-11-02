#include "esp_flasher_uart.h"
#include "esp_flasher_hw.h"

#include <furi.h>

#if ESP_FLASHER_HAS_UART
#include <driver/gpio.h>
#include <driver/uart.h>
#include <furi_hal_power.h>
#include <furi_hal_uart_shared.h>
#endif

#define TAG "EspFlasherUart"

/* Driver ring buffer: one full 1 KB flash block response plus console bursts at 921600. */
#define ESP_FLASHER_UART_DRIVER_RX_BUF (4096)
#define ESP_FLASHER_UART_RX_SLICE_MS   (20)
#define ESP_FLASHER_UART_ACQUIRE_MS    (1000)

struct EspFlasherUart {
    void* context;
    FuriThread* rx_thread;
    FuriMutex* rx_mutex;
    volatile bool rx_claimed;
    volatile bool stop;
    EspFlasherUartRxCallback handle_rx_data_cb;
    /* +1: the console callback NUL-terminates the chunk in place */
    uint8_t rx_buf[RX_BUF_SIZE + 1];
    bool shared_acquired;
    bool driver_installed;
};

void esp_flasher_uart_set_handle_rx_data_cb(EspFlasherUart* uart, EspFlasherUartRxCallback cb) {
    furi_assert(uart);
    uart->handle_rx_data_cb = cb;
}

#if ESP_FLASHER_HAS_UART

static int32_t esp_flasher_uart_rx_thread(void* context) {
    EspFlasherUart* uart = context;

    while(!uart->stop) {
        if(uart->rx_claimed) {
            furi_delay_ms(10);
            continue;
        }

        furi_check(furi_mutex_acquire(uart->rx_mutex, FuriWaitForever) == FuriStatusOk);
        if(uart->rx_claimed || uart->stop) {
            furi_check(furi_mutex_release(uart->rx_mutex) == FuriStatusOk);
            continue;
        }
        const int len = uart_read_bytes(
            ESP_FLASHER_UART_NUM,
            uart->rx_buf,
            RX_BUF_SIZE,
            pdMS_TO_TICKS(ESP_FLASHER_UART_RX_SLICE_MS));
        furi_check(furi_mutex_release(uart->rx_mutex) == FuriStatusOk);

        EspFlasherUartRxCallback cb = uart->handle_rx_data_cb;
        if(len > 0 && cb) cb(uart->rx_buf, (size_t)len, uart->context);
    }

    return 0;
}

bool esp_flasher_uart_tx(EspFlasherUart* uart, const uint8_t* data, size_t len) {
    furi_assert(uart);
    if(len == 0) return true;
    const int written = uart_write_bytes(ESP_FLASHER_UART_NUM, data, len);
    return written >= 0 && (size_t)written == len;
}

bool esp_flasher_uart_set_br(EspFlasherUart* uart, uint32_t baud) {
    furi_assert(uart);
    uart_wait_tx_done(ESP_FLASHER_UART_NUM, pdMS_TO_TICKS(50));
    const esp_err_t err = uart_set_baudrate(ESP_FLASHER_UART_NUM, baud);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "Baud %lu failed: %s", baud, esp_err_to_name(err));
        return false;
    }
    return true;
}

void esp_flasher_uart_claim_rx(EspFlasherUart* uart) {
    furi_assert(uart);
    furi_assert(!uart->rx_claimed);
    /* Flag first so the RX thread does not re-take the mutex after its current slice. */
    uart->rx_claimed = true;
    furi_check(furi_mutex_acquire(uart->rx_mutex, FuriWaitForever) == FuriStatusOk);
}

void esp_flasher_uart_release_rx(EspFlasherUart* uart) {
    furi_assert(uart);
    furi_assert(uart->rx_claimed);
    furi_check(furi_mutex_release(uart->rx_mutex) == FuriStatusOk);
    uart->rx_claimed = false;
}

size_t
    esp_flasher_uart_read(EspFlasherUart* uart, uint8_t* data, size_t size, uint32_t timeout_ms) {
    furi_assert(uart);
    furi_assert(uart->rx_claimed);
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if(timeout_ms && !ticks) ticks = 1;
    const int len = uart_read_bytes(ESP_FLASHER_UART_NUM, data, size, ticks);
    return len > 0 ? (size_t)len : 0;
}

void esp_flasher_uart_flush_rx(EspFlasherUart* uart) {
    furi_assert(uart);
    uart_flush_input(ESP_FLASHER_UART_NUM);
}

static bool esp_flasher_uart_driver_start(void) {
    if(uart_is_driver_installed(ESP_FLASHER_UART_NUM)) {
        FURI_LOG_E(TAG, "UART%d driver already installed", (int)ESP_FLASHER_UART_NUM);
        return false;
    }

    esp_err_t err = uart_driver_install(
        ESP_FLASHER_UART_NUM, ESP_FLASHER_UART_DRIVER_RX_BUF, 0, 0, NULL, 0);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "Driver install failed: %s", esp_err_to_name(err));
        return false;
    }

    err = uart_param_config(
        ESP_FLASHER_UART_NUM,
        &(uart_config_t){
            .baud_rate = BAUDRATE,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        });
    if(err == ESP_OK) {
        err = uart_set_pin(
            ESP_FLASHER_UART_NUM,
            ESP_FLASHER_UART_TX,
            ESP_FLASHER_UART_RX,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE);
    }
    if(err == ESP_OK) {
        /* No target attached must read as idle, not as noise on the console. */
        err = gpio_set_pull_mode((gpio_num_t)ESP_FLASHER_UART_RX, GPIO_PULLUP_ONLY);
    }
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "UART setup failed: %s", esp_err_to_name(err));
        uart_driver_delete(ESP_FLASHER_UART_NUM);
        return false;
    }

    uart_flush_input(ESP_FLASHER_UART_NUM);
    return true;
}

EspFlasherUart* esp_flasher_uart_init(void* context) {
    EspFlasherUart* uart = malloc(sizeof(EspFlasherUart));
    furi_check(uart);
    memset(uart, 0, sizeof(EspFlasherUart));
    uart->context = context;

    if(furi_hal_uart_shared_acquire(ESP_FLASHER_UART_ACQUIRE_MS) !=
       FuriHalUartSharedAcquireAcquired) {
        FURI_LOG_E(TAG, "Shared UART is busy");
        free(uart);
        return NULL;
    }
    uart->shared_acquired = true;
    /* UART RX cannot wake the SoC; keep it awake for the whole session like RFID does. */
    furi_hal_power_light_sleep_inhibit_enter();

    if(!esp_flasher_uart_driver_start()) {
        esp_flasher_uart_free(uart);
        return NULL;
    }
    uart->driver_installed = true;

    uart->rx_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    uart->rx_thread = furi_thread_alloc_ex("EspFlasherUartRx", 3 * 1024, esp_flasher_uart_rx_thread, uart);
    furi_thread_start(uart->rx_thread);

    return uart;
}

void esp_flasher_uart_free(EspFlasherUart* uart) {
    if(!uart) return;

    if(uart->rx_thread) {
        uart->stop = true;
        furi_thread_join(uart->rx_thread);
        furi_thread_free(uart->rx_thread);
    }
    if(uart->rx_mutex) furi_mutex_free(uart->rx_mutex);

    if(uart->driver_installed) {
        uart_wait_tx_done(ESP_FLASHER_UART_NUM, pdMS_TO_TICKS(100));
        const esp_err_t err = uart_driver_delete(ESP_FLASHER_UART_NUM);
        if(err != ESP_OK) FURI_LOG_E(TAG, "Driver delete failed: %s", esp_err_to_name(err));
    }
    if(uart->shared_acquired) {
        /* Also parks TX/RX as pulled-up inputs. */
        if(!furi_hal_uart_shared_release()) FURI_LOG_E(TAG, "Shared UART release failed");
        furi_hal_power_light_sleep_inhibit_exit();
    }

    free(uart);
}

#else /* !ESP_FLASHER_HAS_UART */

bool esp_flasher_uart_tx(EspFlasherUart* uart, const uint8_t* data, size_t len) {
    UNUSED(uart);
    UNUSED(data);
    UNUSED(len);
    return false;
}

bool esp_flasher_uart_set_br(EspFlasherUart* uart, uint32_t baud) {
    UNUSED(uart);
    UNUSED(baud);
    return false;
}

void esp_flasher_uart_claim_rx(EspFlasherUart* uart) {
    UNUSED(uart);
}

void esp_flasher_uart_release_rx(EspFlasherUart* uart) {
    UNUSED(uart);
}

size_t
    esp_flasher_uart_read(EspFlasherUart* uart, uint8_t* data, size_t size, uint32_t timeout_ms) {
    UNUSED(uart);
    UNUSED(data);
    UNUSED(size);
    furi_delay_ms(timeout_ms);
    return 0;
}

void esp_flasher_uart_flush_rx(EspFlasherUart* uart) {
    UNUSED(uart);
}

EspFlasherUart* esp_flasher_uart_init(void* context) {
    UNUSED(context);
    FURI_LOG_E(TAG, "This board has no external UART");
    return NULL;
}

void esp_flasher_uart_free(EspFlasherUart* uart) {
    free(uart);
}

#endif /* ESP_FLASHER_HAS_UART */
