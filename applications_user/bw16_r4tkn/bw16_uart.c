#include "bw16_uart.h"
#include <driver/gpio.h>
#include <furi_hal_power.h>
#include <furi_hal_external_uart.h>
#include <furi_hal_uart_shared.h>
#include <furi_hal_shared_pins.h>

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

/* Ownership follows the other shared-UART users (USB-UART):
 *   1) furi_hal_uart_shared_acquire  - serializes UART users and preempts the
 *                                      background RFID reader
 *   2) furi_hal_shared_pins_acquire  - cooperative GPIO43/44 lease (NRF/IR/RFID)
 *   3) furi_hal_shared_pins_save     - snapshot the pin mux/route state
 *   4) UART2 driver on TX43/RX44
 * bw16_uart_close() undoes them in reverse order. Every step records what it
 * owns, so a failed cleanup keeps the remaining leases and can be retried. */
bool bw16_uart_open(Bw16Uart* io) {
    if(!io) return false;
    if(io->installed || io->awake || io->uart_leased || io->pins_leased || io->pins_saved) {
        io->stage = "UART cleanup pending";
        return false;
    }
    io->stage = "Use BW16 UART firmware";
    io->error = ESP_ERR_NOT_SUPPORTED;
    if(!furi_hal_external_uart_available()) return false;
    io->error = ESP_ERR_INVALID_STATE;
    const FuriHalUartSharedAcquireResult lease = furi_hal_uart_shared_acquire(BW16_UART_LEASE_MS);
    if(lease != FuriHalUartSharedAcquireAcquired) {
        io->stage = lease == FuriHalUartSharedAcquireBusy ? "UART busy: close UART app" :
                                                            "UART lease failed";
        return false;
    }
    io->uart_leased = true;
    io->stage = "Pins busy: close NRF/IR";
    if(!furi_hal_shared_pins_acquire(io)) goto fail;
    io->pins_leased = true;
    /* Current Wardriver GPS takes the UART lease (above), so it already refused
     * us as busy. Older Wardriver FAPs and other legacy apps install UART1/UART2
     * without any lease: never reconfigure an already-installed driver, and
     * check before the snapshot so a busy failure leaves their pins untouched. */
    io->stage = "UART busy: close GPS";
    if(uart_is_driver_installed(UART_NUM_1) || uart_is_driver_installed(BW16_UART)) goto fail;
    io->stage = "UART pin save failed";
    if(!furi_hal_shared_pins_save(io)) goto fail;
    io->pins_saved = true;
    io->tx_failed = false;
    io->stage = "UART install failed";
    io->error = uart_driver_install(BW16_UART, 8192, 0, 64, &io->events, 0);
    if(io->error != ESP_OK) goto fail;
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
    }
    /* Put GPIO43/44 back exactly as found and drop the GPIO reservation that
     * uart_set_pin() added. The lease cannot be released while saved. */
    if(io->pins_saved) {
        furi_hal_shared_pins_restore(io);
        if(furi_hal_shared_pins_is_saved(io)) return ESP_FAIL;
        io->pins_saved = false;
    }
    if(io->uart_leased) {
        if(!furi_hal_uart_shared_release()) return ESP_FAIL;
        io->uart_leased = false;
    }
    if(io->pins_leased) {
        furi_hal_shared_pins_release(io);
        io->pins_leased = false;
    }
    if(io->awake) {
        furi_hal_power_insomnia_exit();
        io->awake = false;
    }
    return ESP_OK;
}
