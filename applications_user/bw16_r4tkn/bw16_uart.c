#include "bw16_uart.h"
#include <driver/gpio.h>
#include <furi_hal_power.h>
#include <furi_hal_external_uart.h>

/* Leave RX as an input even with BW16 attached. A high TX idle prevents BREAKs.
 * The personal firmware reserves the header; there are no NRF/SPI accesses. */
static esp_err_t park_pins(void) {
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BW16_TX) | (1ULL << BW16_RX),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

bool bw16_uart_open(Bw16Uart* io) {
    if(!io || io->installed || io->awake) return false;
    io->stage = "Use BW16 UART firmware";
    io->error = ESP_ERR_NOT_SUPPORTED;
    if(!furi_hal_external_uart_available()) return false;
    io->stage = "UART busy: close GPS";
    io->error = ESP_ERR_INVALID_STATE;
    /* UART1 also serves optional RFID/configurable GPS. The loader runs one
     * foreground app; never reconfigure an already-installed driver. */
    if(uart_is_driver_installed(UART_NUM_1) || uart_is_driver_installed(BW16_UART)) return false;
    io->tx_failed = false;
    io->stage = "UART install failed";
    io->error = uart_driver_install(BW16_UART, 8192, 0, 64, &io->events, 0);
    if(io->error != ESP_OK) return false;
    io->installed = true;
    furi_hal_power_insomnia_enter();
    io->awake = true;
    const uart_config_t cfg = {
        .baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    io->stage = "UART config failed";
    io->error = uart_param_config(BW16_UART, &cfg);
    if(io->error != ESP_OK) goto fail;
    io->stage = "UART GPIO setup failed";
    io->error = park_pins();
    if(io->error != ESP_OK) goto fail;
    io->error = uart_set_pin(BW16_UART, BW16_TX, BW16_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if(io->error != ESP_OK) goto fail;
    /* Flush once before synchronization, never while receiving a reply. */
    io->stage = "UART RX setup failed";
    io->error = uart_flush_input(BW16_UART);
    if(io->error != ESP_OK) goto fail;
    io->stage = "UART ready";
    return true;
fail:
    bw16_uart_close(io);
    return false;
}

esp_err_t bw16_uart_send(Bw16Uart* io, const char* line, size_t size) {
    if(!io || !io->installed || io->tx_failed) return ESP_ERR_INVALID_STATE;
    if(!line || size < 2 || size > 48 || line[size - 1] != '\n') return ESP_ERR_INVALID_ARG;
    for(size_t i = 0; i + 1 < size; ++i)
        if((unsigned char)line[i] < 32 || (unsigned char)line[i] > 126) return ESP_ERR_INVALID_ARG;
    /* Both routes remain fixed throughout the session, including idle RX. */
    const int written = uart_write_bytes(BW16_UART, line, size);
    io->error = uart_wait_tx_done(BW16_UART, pdMS_TO_TICKS(250));
    if(io->error == ESP_OK && written != (int)size) io->error = ESP_FAIL;
    if(io->error != ESP_OK) {
        io->tx_failed = true;
        io->stage = "UART TX failed";
        park_pins();
    }
    return io->error;
}

esp_err_t bw16_uart_close(Bw16Uart* io) {
    if(!io) return ESP_ERR_INVALID_ARG;
    if(io->installed) {
        esp_err_t error = uart_driver_delete(BW16_UART);
        if(error != ESP_OK) return error;
        io->installed = false;
        io->events = NULL;
    }
    /* Busy failure must leave GPS pins alone. Awake remains set until cleanup
     * succeeds, allowing the UI to retry without abandoning owned resources. */
    if(io->awake) {
        esp_err_t error = park_pins();
        if(error != ESP_OK) return error;
        furi_hal_power_insomnia_exit();
        io->awake = false;
    }
    return ESP_OK;
}
