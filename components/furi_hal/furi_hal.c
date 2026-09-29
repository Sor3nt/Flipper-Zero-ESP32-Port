#include "furi_hal.h"
#include "boards/board.h"
#include <furi_hal_gpio.h>
#include <esp_log.h>
#include "furi_hal_external_uart.h"
#include <nvs_flash.h>
#include <furi_hal_sd.h>

static const char* TAG = "FuriHal";

bool furi_hal_external_uart_available(void) {
#if CONFIG_T_EMBED_EXTERNAL_UART && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG && defined(BOARD_EXTERNAL_UART_TX)
    return true;
#else
    return false;
#endif
}

void furi_hal_init_early(void) {
    furi_hal_cortex_init_early();

#if CONFIG_T_EMBED_PLUS_SD_RECOVERY
#if BOARD_PIN_SD_CS != 13 || BOARD_PIN_NRF24_CSN != 44 || BOARD_PIN_NRF24_CE != 43 || !BOARD_CC1101_SPI_SHARED
#error "Plus SD recovery requires the T-Embed CC1101 shared-SPI pin layout"
#endif
    /* Preload every shared-bus select before enabling its output. A reset
     * need not reset the peripherals: keep them deselected before power-up
     * and before any SPI clock, including the CC1101 probe. */
    const GpioPin* shared_selects[] = {
        &gpio_sdcard_cs, &gpio_lcd_cs, &gpio_ext_pa4, &gpio_nrf24_cs,
    };
    for(size_t i = 0; i < sizeof(shared_selects) / sizeof(shared_selects[0]); ++i) {
        furi_hal_gpio_write(shared_selects[i], true);
        furi_hal_gpio_init_simple(shared_selects[i], GpioModeOutputPushPull);
    }
    static const GpioPin nrf24_ce_idle = {.port = NULL, .pin = BOARD_PIN_NRF24_CE};
    furi_hal_gpio_write(&nrf24_ce_idle, false);
    furi_hal_gpio_init_simple(&nrf24_ce_idle, GpioModeOutputPushPull);
#endif

#ifdef BOARD_PIN_PWR_EN
    /* Power-enable must be set early — powers CC1101, BQ27220 fuel gauge, WS2812 */
    static const GpioPin pwr_en = {.port = NULL, .pin = BOARD_PIN_PWR_EN};
    furi_hal_gpio_init_simple(&pwr_en, GpioModeOutputPushPull);
    furi_hal_gpio_write(&pwr_en, true);
    ESP_LOGI(TAG, "PWR_EN GPIO%d set HIGH", BOARD_PIN_PWR_EN);
#endif

#if CONFIG_T_EMBED_EXTERNAL_UART && defined(BOARD_EXTERNAL_UART_TX)
    /* External UART profile: never drive the connected module's TX pin.
     * Keep a high idle on host TX until GPS/BW16 installs its UART. */
    static const GpioPin external_tx = {.port = NULL, .pin = BOARD_EXTERNAL_UART_TX};
    static const GpioPin external_rx = {.port = NULL, .pin = BOARD_EXTERNAL_UART_RX};
    furi_hal_gpio_init(&external_tx, GpioModeInput, GpioPullUp, GpioSpeedLow);
    furi_hal_gpio_init(&external_rx, GpioModeInput, GpioPullUp, GpioSpeedLow);
#elif defined(BOARD_PIN_NRF24_CSN) && !CONFIG_T_EMBED_PLUS_SD_RECOVERY
    /* T-Embed Plus shares SPI2 between CC1101 and NRF24. Drive NRF24 CSN HIGH
     * (deselected) and CE LOW (standby) at boot, before any CC1101 SPI traffic.
     * Without this, NRF24 sees CC1101 traffic and corrupts the bus, manifesting
     * as a stuck ~312 MHz reading in the Frequency Analyzer and total RX failure. */
    static const GpioPin nrf24_csn = {.port = NULL, .pin = BOARD_PIN_NRF24_CSN};
    furi_hal_gpio_init_simple(&nrf24_csn, GpioModeOutputPushPull);
    furi_hal_gpio_write(&nrf24_csn, true);
    ESP_LOGI(TAG, "NRF24_CSN GPIO%d set HIGH (deselect)", BOARD_PIN_NRF24_CSN);
#endif

#if defined(BOARD_PIN_NRF24_CE) && !(CONFIG_T_EMBED_EXTERNAL_UART && defined(BOARD_EXTERNAL_UART_TX)) && !CONFIG_T_EMBED_PLUS_SD_RECOVERY
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
    /* NVS is required by WiFi and BLE — init once at boot */
    esp_err_t nvs_err = nvs_flash_init();
    if(nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    furi_hal_rtc_init();
    furi_hal_version_init();
    furi_hal_info_init();
    furi_hal_power_init();
    furi_hal_crypto_init();
#if CONFIG_T_EMBED_PLUS_SD_RECOVERY
    /* Adding the radio handle initializes SPI2 but sends no radio commands.
     * The SD card must enter SPI mode before either radio or LCD traffic.
     * No filesystem is mounted here; the storage service still owns it. */
    furi_hal_spi_bus_handle_init(&furi_hal_spi_bus_handle_subghz);
    furi_delay_ms(100);
    if(furi_hal_sd_init(false) != FuriStatusOk) {
        ESP_LOGW(TAG, "Early SD initialization failed; storage will retry");
    }
#endif
    furi_hal_subghz_init();
    furi_hal_usb_init();
    furi_hal_light_init();
    furi_hal_display_init();
    furi_hal_speaker_init();
    furi_hal_nfc_init();
    ESP_LOGI(TAG, "Init complete");
}
