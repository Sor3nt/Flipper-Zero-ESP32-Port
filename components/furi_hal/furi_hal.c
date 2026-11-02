#include "furi_hal.h"
#include <furi_hal_sd.h>
#include "boards/board.h"
#include <furi_hal_gpio.h>
#include <furi_hal_bt.h>
#include "furi_hal_uart_shared.h"
#include <esp_log.h>
#include <nvs_flash.h>

static const char* TAG = "FuriHal";

void furi_hal_init_early(void) {
    furi_hal_cortex_init_early();

#if defined(BOARD_CC1101_SPI_SHARED) && BOARD_CC1101_SPI_SHARED && \
    defined(BOARD_HAS_SD_CARD) && BOARD_HAS_SD_CARD
    /* Keep the LCD and SD card deselected before any shared-SPI traffic. */
    static const GpioPin lcd_cs = {.port = NULL, .pin = BOARD_PIN_LCD_CS};
    static const GpioPin sd_cs = {.port = NULL, .pin = BOARD_PIN_SD_CS};
    furi_hal_gpio_init_simple(&lcd_cs, GpioModeOutputPushPull);
    furi_hal_gpio_write(&lcd_cs, true);
    furi_hal_gpio_init_simple(&sd_cs, GpioModeOutputPushPull);
    furi_hal_gpio_write(&sd_cs, true);
#endif

#ifdef BOARD_PIN_PWR_EN
    /* Power-enable must be set early — powers CC1101, BQ27220 fuel gauge, WS2812 */
    static const GpioPin pwr_en = {.port = NULL, .pin = BOARD_PIN_PWR_EN};
    furi_hal_gpio_init_simple(&pwr_en, GpioModeOutputPushPull);
    furi_hal_gpio_write(&pwr_en, true);
    ESP_LOGI(TAG, "PWR_EN GPIO%d set HIGH", BOARD_PIN_PWR_EN);
#endif

#ifdef BOARD_PIN_NRF24_CSN
    /* Shared-SPI boards route CC1101 and NRF24 to one host. Drive NRF24 CSN HIGH
     * (deselected) and CE LOW (standby) at boot, before any radio SPI traffic.
     * Without this, NRF24 sees CC1101 traffic and corrupts the bus, manifesting
     * as a stuck ~312 MHz reading in the Frequency Analyzer and total RX failure. */
    static const GpioPin nrf24_csn = {.port = NULL, .pin = BOARD_PIN_NRF24_CSN};
    furi_hal_gpio_init_simple(&nrf24_csn, GpioModeOutputPushPull);
    furi_hal_gpio_write(&nrf24_csn, true);
    ESP_LOGI(TAG, "NRF24_CSN GPIO%d set HIGH (deselect)", BOARD_PIN_NRF24_CSN);
#endif

#ifdef BOARD_PIN_CC1101_CSN
    /* Keep the CC1101 deselected before the radio SPI host is configured. Without this,
     * the radio can sample boot-time GPIO/SPI activity as a command. */
    static const GpioPin cc1101_csn = {.port = NULL, .pin = BOARD_PIN_CC1101_CSN};
    furi_hal_gpio_init_simple(&cc1101_csn, GpioModeOutputPushPull);
    furi_hal_gpio_write(&cc1101_csn, true);
    ESP_LOGI(TAG, "CC1101_CSN GPIO%d set HIGH (deselect)", BOARD_PIN_CC1101_CSN);
#endif

#ifdef BOARD_PIN_CC1101_GDO0
    #if BOARD_PIN_CC1101_GDO0 != UINT16_MAX
    static const GpioPin cc1101_gdo0 = {.port = NULL, .pin = BOARD_PIN_CC1101_GDO0};
    furi_hal_gpio_init_simple(&cc1101_gdo0, GpioModeInput);
    #endif
#endif

#ifdef BOARD_PIN_NRF24_CE
    static const GpioPin nrf24_ce = {.port = NULL, .pin = BOARD_PIN_NRF24_CE};
    furi_hal_gpio_init_simple(&nrf24_ce, GpioModeOutputPushPull);
    furi_hal_gpio_write(&nrf24_ce, false);
    ESP_LOGI(TAG, "NRF24_CE GPIO%d set LOW (standby)", BOARD_PIN_NRF24_CE);
#endif

    ESP_LOGI(TAG, "Early init complete");
}

void furi_hal_deinit_early(void) {
}

void furi_hal_init(void) {
    furi_hal_uart_shared_init();
    furi_hal_crypto_init();

    /* NVS is required by WiFi and BLE — init once at boot */
    esp_err_t nvs_err = nvs_flash_init();
    if(nvs_err == ESP_OK || nvs_err == ESP_ERR_INVALID_STATE) {
        if(!furi_hal_crypto_u2f_storage_key_migrate()) {
            ESP_LOGE(TAG, "Unable to migrate the U2F storage key");
        }
    }
    if(nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(
            TAG,
            "NVS recovery will erase the default partition; a legacy U2F key not yet migrated cannot be recovered");
        nvs_err = nvs_flash_erase();
        if(nvs_err == ESP_OK) {
            nvs_err = nvs_flash_init();
        }
    }
    if(nvs_err != ESP_OK) ESP_LOGE(TAG, "NVS initialization failed: %s", esp_err_to_name(nvs_err));

    furi_hal_rtc_init();
    furi_hal_version_init();
    furi_hal_info_init();
    furi_hal_spi_init();
    furi_hal_power_init();
#if defined(BOARD_CC1101_SPI_SHARED) && BOARD_CC1101_SPI_SHARED && \
    defined(BOARD_HAS_SD_CARD) && BOARD_HAS_SD_CARD
    /* Initialize the shared SPI2 host and attach the CC1101 without sending
     * commands, then initialize the SD card before the first radio/LCD traffic. */
    furi_hal_spi_bus_handle_init(&furi_hal_spi_bus_handle_subghz);
    if(furi_hal_spi_bus_handle_subghz.initialized) {
        const FuriStatus sd_status = furi_hal_sd_init(false);
        if(sd_status != FuriStatusOk) {
            ESP_LOGW(TAG, "Early shared-bus SD initialization failed; storage will retry later");
        }
    } else {
        ESP_LOGW(TAG, "Skipping early SD initialization because SPI2 did not initialize");
    }
#endif
    furi_hal_bt_init();
    furi_hal_infrared_init();
    furi_hal_subghz_init();
    furi_hal_usb_init();
    furi_hal_light_init();
    furi_hal_display_init();
    furi_hal_speaker_init();
    furi_hal_nfc_init();
    ESP_LOGI(TAG, "Init complete");
}
