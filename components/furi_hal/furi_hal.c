#include "furi_hal.h"
#include "boards/board.h"
#include <furi_hal_gpio.h>
#include "furi_hal_uart_shared.h"
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

bool furi_hal_usb_uart_bridge_available(void) {
#if !(CONFIG_IDF_TARGET_ESP32S3 && defined(BOARD_HAS_USB_UART_BRIDGE) && BOARD_HAS_USB_UART_BRIDGE && \
      defined(BOARD_USB_UART_NUM) && defined(BOARD_USB_UART_TX) && defined(BOARD_USB_UART_RX) && \
      BOARD_USB_UART_NUM >= 0 && BOARD_USB_UART_TX >= 0 && BOARD_USB_UART_RX >= 0)
    /* Board has no USB-UART bridge mapping (or is not an ESP32-S3). */
    return false;
#elif defined(BOARD_PIN_NRF24_CE) && defined(BOARD_PIN_NRF24_CSN) &&                     \
    (BOARD_PIN_NRF24_CE == BOARD_USB_UART_TX || BOARD_PIN_NRF24_CE == BOARD_USB_UART_RX || \
     BOARD_PIN_NRF24_CSN == BOARD_USB_UART_TX || BOARD_PIN_NRF24_CSN == BOARD_USB_UART_RX)
    /* The bridge pads overlap the NRF24 control pins (T-Embed CC1101 routes
     * CE=GPIO43 / CSN=GPIO44 onto the UART1 pads). Driving them as a UART while
     * the NRF24 is present corrupts the shared SPI select lines, so the bridge
     * is only safe in the external-UART profile that declares the NRF absent. */
    return furi_hal_external_uart_available();
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* Dedicated bridge pads with no NRF24 overlap (e.g. Paradoxt: NRF on
     * GPIO13/14, UART1 on GPIO43/44). Available whenever the log console runs
     * over USB Serial/JTAG, so UART0 is not driving GPIO43/44. */
    return true;
#else
    /* UART0 console would fight the bridge for the pads. */
    return false;
#endif
}

bool furi_hal_usb_uart_bridge_get_pins(int* uart_num, int* tx, int* rx) {
    if(!furi_hal_usb_uart_bridge_available()) return false;
#if defined(BOARD_USB_UART_NUM) && defined(BOARD_USB_UART_TX) && defined(BOARD_USB_UART_RX)
    if(uart_num) *uart_num = BOARD_USB_UART_NUM;
    if(tx) *tx = BOARD_USB_UART_TX;
    if(rx) *rx = BOARD_USB_UART_RX;
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
    /* GPIO ISRs are allocated on the core that installs the service: do it
     * here, on the boot core, rather than in whichever thread asks first. */
    furi_hal_gpio_init_isr_service();
    furi_hal_uart_shared_init();
    furi_hal_crypto_init();

    /* NVS is required by WiFi and BLE — init once at boot */
    esp_err_t nvs_err = nvs_flash_init();
    if(nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        esp_err_t erase_err = nvs_flash_erase();
        nvs_err = erase_err == ESP_OK ? nvs_flash_init() : erase_err;
    }
    if(nvs_err == ESP_OK || nvs_err == ESP_ERR_INVALID_STATE) {
        if(!furi_hal_crypto_u2f_storage_key_migrate()) {
            ESP_LOGE(TAG, "Unable to migrate the U2F storage key");
        }
    } else {
        ESP_LOGE(TAG, "NVS initialization failed: %s", esp_err_to_name(nvs_err));
    }

    furi_hal_rtc_init();
    furi_hal_version_init();
    furi_hal_info_init();
    furi_hal_power_init();
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
