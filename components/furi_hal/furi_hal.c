#include "furi_hal.h"
#include "boards/board.h"
#include <furi_hal_gpio.h>
#include <esp_log.h>
#include <nvs_flash.h>

static const char* TAG = "FuriHal";

void furi_hal_init_early(void) {
    furi_hal_cortex_init_early();

#ifdef BOARD_PIN_PWR_EN
    /* Power-enable must be set early — powers CC1101, BQ27220 fuel gauge, WS2812 */
    static const GpioPin pwr_en = {.port = NULL, .pin = BOARD_PIN_PWR_EN};
    furi_hal_gpio_init_simple(&pwr_en, GpioModeOutputPushPull);
    furi_hal_gpio_write(&pwr_en, true);
    ESP_LOGI(TAG, "PWR_EN GPIO%d set HIGH", BOARD_PIN_PWR_EN);
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
    furi_hal_power_init();
    furi_hal_crypto_init();
    furi_hal_subghz_init();
    furi_hal_usb_init();
    furi_hal_light_init();
    furi_hal_display_init();
    furi_hal_speaker_init();
    furi_hal_nfc_init();
    ESP_LOGI(TAG, "Init complete");
}
