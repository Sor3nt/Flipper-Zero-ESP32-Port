#include "bw16_uart.h"
#include <furi_hal_shared_pins.h>
#include <furi_hal_power.h>
#include <driver/gpio.h>
#include <sdkconfig.h>

bool bw16_uart_open(Bw16Uart* io) {
    io->stage = "USB console required";
    io->error = ESP_ERR_NOT_SUPPORTED;
#if !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    return false;
#else
    io->stage = "Pins busy: NRF/RFID";
    io->error = ESP_ERR_INVALID_STATE;
    if(!furi_hal_shared_pins_acquire(io)) return false;
    io->leased = true;
    furi_hal_power_insomnia_enter();
    io->awake = true;
    io->stage = "UART1 already owned";
    if(uart_is_driver_installed(UART_NUM_1)) goto fail;
    io->stage = "Board/pin snapshot";
    if(!furi_hal_shared_pins_save(io)) goto fail;
    io->saved = true;
    io->stage = "NRF guard / SPI setup";
    io->error = furi_hal_bw16_guard_open(&io->guard, io);
    if(io->error != ESP_OK) goto fail;
    /* The driver must be ours before changing parameters or routes. */
    io->stage = "UART install/memory";
    io->error = uart_driver_install(UART_NUM_1, 4096, 0, 64, &io->events, 0);
    if(io->error != ESP_OK) goto fail;
    io->installed = true;
    uart_config_t cfg = {
        .baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    io->stage = "UART configuration";
    io->error = uart_param_config(UART_NUM_1, &cfg);
    if(io->error != ESP_OK) goto fail;
    /* GPIO43 starts as NRF CE output. Release it before attaching BW16 TX.
     * SPI is held throughout this transition; NRF is verified powered down. */
    gpio_config_t pins = {
        .pin_bit_mask = (1ULL << 43) | (1ULL << 44),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    io->stage = "Release GPIO outputs";
    io->error = gpio_config(&pins);
    if(io->error != ESP_OK) goto fail;
    io->stage = "UART GPIO43/44";
    io->error = uart_set_pin(UART_NUM_1, 44, 43, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if(io->error != ESP_OK) goto fail;
    io->stage = "UART RX flush";
    io->error = uart_flush_input(UART_NUM_1);
    if(io->error != ESP_OK) goto fail;
    io->stage = "NRF CS idle check";
    io->error = furi_hal_bw16_guard_ready(&io->guard);
    if(io->error != ESP_OK) goto fail;
    return true;
fail:
    bw16_uart_close(io); /* BW16 is still unplugged on setup failure. */
    return false;
#endif
}

bool bw16_uart_scan(Bw16Uart* io) {
    if(!io->installed) return false;
    io->stage = "Protected UART TX failed";
    io->error = furi_hal_bw16_guard_scan(&io->guard);
    return io->error == ESP_OK;
}

esp_err_t bw16_uart_close(Bw16Uart* io) {
    if(io->guard.owner) {
        esp_err_t err = furi_hal_bw16_guard_close(&io->guard);
        if(err != ESP_OK) return err;
    }
    if(io->installed) {
        esp_err_t err = uart_driver_delete(UART_NUM_1);
        if(err != ESP_OK) return err; /* Do not release another usable owner. */
        io->installed = false;
        io->events = NULL; /* The UART driver owns/deletes this queue. */
    }
    if(io->saved) { furi_hal_shared_pins_restore(io); io->saved = false; }
    if(io->awake) { furi_hal_power_insomnia_exit(); io->awake = false; }
    if(io->leased) { furi_hal_shared_pins_release(io); io->leased = false; }
    return ESP_OK;
}
