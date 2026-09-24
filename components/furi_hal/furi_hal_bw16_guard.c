#include "furi_hal_bw16_guard.h"
#include <sdkconfig.h>
#include "furi_hal_shared_pins.h"
#include "boards/board.h"
#include <string.h>

#if CONFIG_IDF_TARGET_ESP32S3 && BOARD_PIN_NRF24_CE == 43 && BOARD_PIN_NRF24_CSN == 44
#include "furi_hal_spi_bus.h"
#include <driver/spi_master.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <esp_rom_gpio.h>
#include <esp_rom_sys.h>
#include <esp_log.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_periph.h>
#include <soc/gpio_sig_map.h>
#include <soc/io_mux_reg.h>

/* A no-CS device also acquires IDF's hardware bus lock. The Furi mutex alone
 * would not drain outstanding LCD DMA after a display completion timeout. */
static esp_err_t take(FuriHalBw16Guard* g) {
    if(g->locked) return ESP_OK;
    furi_hal_spi_bus_lock();
    esp_err_t err = spi_device_acquire_bus(g->device, portMAX_DELAY);
    if(err != ESP_OK) { furi_hal_spi_bus_unlock(); return err; }
    g->locked = true;
    return ESP_OK;
}

static void give(FuriHalBw16Guard* g) {
    if(!g->locked) return;
    spi_device_release_bus(g->device);
    g->locked = false;
    furi_hal_spi_bus_unlock();
}

/* Fixed, valid S3 pads: register-level parking cannot leave a timed-out UART
 * shifter connected to CSN. Preload the level before switching its route. */
static void park(unsigned pin, unsigned level) {
    gpio_ll_set_level(&GPIO, pin, level);
    gpio_ll_iomux_func_sel(GPIO_PIN_MUX_REG[pin], PIN_FUNC_GPIO);
    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_output_enable(&GPIO, pin);
}

static esp_err_t reg(FuriHalBw16Guard* g, unsigned address, bool write, uint8_t* value) {
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length = 16,
        .tx_data = {(write ? 0x20 : 0) | address, write ? *value : 0xff},
    };
    gpio_set_level(44, 0);
    esp_rom_delay_us(1);
    esp_err_t err = spi_device_polling_transmit(g->device, &t);
    gpio_set_level(44, 1);
    esp_rom_delay_us(1);
    if(err == ESP_OK && (t.rx_data[0] & 0x80)) return ESP_ERR_INVALID_RESPONSE;
    if(err == ESP_OK && !write) *value = t.rx_data[1];
    return err;
}

static esp_err_t config(FuriHalBw16Guard* g, uint8_t value) {
    uint8_t readback = 0xff;
    esp_err_t err = reg(g, 0, true, &value);
    if(err == ESP_OK) err = reg(g, 0, false, &readback);
    if(err == ESP_OK && readback != value) err = ESP_ERR_INVALID_RESPONSE;
    return err;
}

esp_err_t furi_hal_bw16_guard_open(FuriHalBw16Guard* g, const void* owner) {
    if(!g || g->owner || !furi_hal_shared_pins_is_saved(owner)) return ESP_ERR_INVALID_STATE;
    g->owner = owner;
    spi_device_interface_config_t cfg = {
        .clock_speed_hz = 1000000, .mode = 0, .spics_io_num = -1, .queue_size = 1,
    };
    spi_device_handle_t device = NULL;
    furi_hal_spi_bus_lock();
    esp_err_t err = spi_bus_add_device(SPI2_HOST, &cfg, &device);
    furi_hal_spi_bus_unlock();
    if(err != ESP_OK) return err;
    g->device = device;
    if((err = take(g)) != ESP_OK) return err;
    park(44, 1);
    park(43, 0);
    gpio_config_t pins = {
        .pin_bit_mask = (1ULL << 43) | (1ULL << 44),
        .mode = GPIO_MODE_INPUT_OUTPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    if((err = gpio_config(&pins)) != ESP_OK) return err;
    uint8_t original = 0xff, width = 0;
    if((err = reg(g, 0, false, &original)) != ESP_OK) return err;
    if((err = reg(g, 3, false, &width)) != ESP_OK) return err;
    if((original & 0x80) || width < 1 || width > 3) return ESP_ERR_INVALID_RESPONSE;
    g->config_down = original & ~0x02; /* PWR_UP=0 makes CE activity inert. */
    g->config_saved = true;
    /* Verify a writable register, rejecting absent/stuck SPI responses. Both
     * values keep the radio powered down; only MASK_RX_DR is toggled briefly.
     * Do not use EN_CRC here: auto-ack can force CRC on independently. */
    if((err = config(g, g->config_down ^ 0x40)) != ESP_OK) return err;
    return config(g, g->config_down);
}

esp_err_t furi_hal_bw16_guard_ready(FuriHalBw16Guard* g) {
    if(!g || !g->locked || !g->config_saved) return ESP_ERR_INVALID_STATE;
    if(!gpio_get_level(44)) return ESP_ERR_INVALID_STATE;
    esp_rom_delay_us(1); /* CSN high -> MISO high-Z before releasing SPI. */
    g->ready = true;
    give(g);
    return ESP_OK;
}

esp_err_t furi_hal_bw16_guard_scan(FuriHalBw16Guard* g) {
    return furi_hal_bw16_guard_send(g, "SCAN\n", 5);
}

esp_err_t furi_hal_bw16_guard_send(FuriHalBw16Guard* g, const char* line, size_t size) {
    if(!line || size < 2 || size > 48 || line[size - 1] != '\n') return ESP_ERR_INVALID_ARG;
    for(size_t i = 0; i + 1 < size; ++i)
        if((unsigned char)line[i] < 32 || (unsigned char)line[i] > 126) return ESP_ERR_INVALID_ARG;
    if(!g || !g->ready || g->failed || !furi_hal_shared_pins_is_saved(g->owner))
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = take(g);
    if(err != ESP_OK) return err;
    int written = uart_write_bytes(UART_NUM_1, line, size);
    /* A short write can still start TX, so ALWAYS drain or park before giving
     * SPI back. The hardware arbiter keeps LCD, SD and CC1101 clocks stopped. */
    err = uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(250));
    if(err == ESP_OK && written != (int)size) err = ESP_FAIL;
    if(err == ESP_OK && !gpio_get_level(44)) err = ESP_ERR_INVALID_STATE;
    if(err != ESP_OK) {
        park(44, 1);
        g->failed = true; /* Never reconnect this UART shifter until reopened. */
    }
    esp_rom_delay_us(1);
    give(g);
    return err;
}

esp_err_t furi_hal_bw16_guard_close(FuriHalBw16Guard* g) {
    if(!g || !g->owner) return ESP_OK;
    if(!furi_hal_shared_pins_is_saved(g->owner)) return ESP_ERR_INVALID_STATE;
    g->ready = false;
    if(g->device) {
        esp_err_t err = take(g);
        if(err != ESP_OK) return err;
        /* Caller confirmed BW16 disconnected: CE may now be driven LOW. */
        park(44, 1);
        park(43, 0);
        if(g->config_saved) {
            err = config(g, g->config_down);
            if(err != ESP_OK) ESP_LOGW("BW16Guard", "NRF CONFIG restore failed: %d", err);
        }
        /* On a missing/broken NRF, still release after parking CE/CS. No BW16
         * is attached now; the NRF application can reinitialize the radio. */
        give(g);
        furi_hal_spi_bus_lock();
        err = spi_bus_remove_device(g->device);
        furi_hal_spi_bus_unlock();
        if(err != ESP_OK) return err;
    }
    memset(g, 0, sizeof(*g));
    return ESP_OK;
}
#else
esp_err_t furi_hal_bw16_guard_open(FuriHalBw16Guard* g, const void* owner) {
    (void)g; (void)owner; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t furi_hal_bw16_guard_ready(FuriHalBw16Guard* g) { (void)g; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t furi_hal_bw16_guard_scan(FuriHalBw16Guard* g) { (void)g; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t furi_hal_bw16_guard_send(FuriHalBw16Guard* g, const char* line, size_t size) {
    (void)g; (void)line; (void)size; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t furi_hal_bw16_guard_close(FuriHalBw16Guard* g) { (void)g; return ESP_ERR_NOT_SUPPORTED; }
#endif
