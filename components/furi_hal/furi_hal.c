#include "furi_hal.h"
#include "boards/board.h"
#include <furi_hal_gpio.h>
#include <esp_log.h>
#include <nvs_flash.h>

static const char* TAG = "FuriHal";

#ifdef BOARD_HAS_M5PM1
#include <driver/i2c.h>
#include <freertos/FreeRTOS.h>

/* Bring up the M5PM1 PMIC (StickS3). Mirrors M5GFX's M5StickS3 boot sequence:
 * disable the PMIC I2C idle-sleep so it stays responsive, then drive M5PM1 GPIO2
 * high to enable the LCD power rail ("L3B Enable, LCD Power On"). Must run before
 * the display is initialized, or the panel is unpowered and the screen is dark. */
static void furi_hal_m5pm1_init(void) {
    const i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_PMIC_SDA,
        .scl_io_num = BOARD_PMIC_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    if(i2c_param_config(BOARD_PMIC_I2C_PORT, &conf) != ESP_OK) return;
    esp_err_t e = i2c_driver_install(BOARD_PMIC_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if(e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "M5PM1 i2c install failed: %s", esp_err_to_name(e));
        return;
    }

/* The PMIC powers up with I2C idle-sleep enabled, so the first transaction after
 * reset only serves to wake it and reliably times out (and the one after it can
 * still NACK). Retry each transfer instead of letting a write silently drop —
 * losing the 0x09 write in particular leaves idle-sleep on, which lets the PMIC
 * reset its rails and blank the panel some time after a good boot. */
#define PM1_RETRY(expr)                                                       \
    ({                                                                        \
        esp_err_t _e = ESP_FAIL;                                              \
        for(int _try = 0; _try < 4; _try++) {                                 \
            _e = (expr);                                                      \
            if(_e == ESP_OK) break;                                           \
            vTaskDelay(pdMS_TO_TICKS(5));                                     \
        }                                                                     \
        _e;                                                                   \
    })
#define PM1_WRITE(reg, val)                                                   \
    PM1_RETRY(({                                                              \
        uint8_t _b[2] = {(uint8_t)(reg), (uint8_t)(val)};                     \
        i2c_master_write_to_device(                                           \
            BOARD_PMIC_I2C_PORT, BOARD_PMIC_ADDR, _b, 2, pdMS_TO_TICKS(50));  \
    }))
#define PM1_READ(reg, out)                                                    \
    PM1_RETRY(({                                                              \
        uint8_t _r = (uint8_t)(reg);                                          \
        i2c_master_write_read_device(                                         \
            BOARD_PMIC_I2C_PORT, BOARD_PMIC_ADDR, &_r, 1, (out), 1,           \
            pdMS_TO_TICKS(50));                                               \
    }))

    uint8_t v = 0;
    /* Read device ID (reg 0x00) to confirm the chip actually responds at 0x6E. */
    esp_err_t idr = PM1_READ(0x00, &v);
    ESP_LOGI(TAG, "M5PM1 devID(0x00) read: err=%s val=0x%02X", esp_err_to_name(idr), v);

    /* Disable I2C idle sleep (M5GFX reg 0x09 = 0). */
    esp_err_t e09 = PM1_WRITE(0x09, 0x00);

    /* Enable the LCD power rail via M5PM1 GPIO2 (M5GFX "L3B Enable, LCD Power On"). */
    esp_err_t e16 = PM1_READ(0x16, &v); v &= (uint8_t)~(1u << 2); e16 = PM1_WRITE(0x16, v);
    esp_err_t e10 = PM1_READ(0x10, &v); v |= (uint8_t)(1u << 2);  e10 = PM1_WRITE(0x10, v);
    esp_err_t e13 = PM1_READ(0x13, &v); v &= (uint8_t)~(1u << 2); e13 = PM1_WRITE(0x13, v);
    esp_err_t e11 = PM1_READ(0x11, &v); v |= (uint8_t)(1u << 2);  e11 = PM1_WRITE(0x11, v);
    /* Enable the Port A / Grove 5V output (M5PM1 reg 0x06 bit3 = 1). This powers
     * external Grove units (RFID2/NFC readers) on the side connector. Without it
     * the Grove port is dead and any attached I2C unit never ACKs. Mirrors
     * M5Unified Power_Class::setExtOutput() for board_M5StickS3. */
    esp_err_t e06 = PM1_READ(0x06, &v); v |= (uint8_t)(1u << 3); e06 = PM1_WRITE(0x06, v);
    ESP_LOGI(TAG, "M5PM1 writes err: 0x09=%s 0x16=%s 0x10=%s 0x13=%s 0x11=%s 0x06(PortA5V)=%s",
             esp_err_to_name(e09), esp_err_to_name(e16), esp_err_to_name(e10),
             esp_err_to_name(e13), esp_err_to_name(e11), esp_err_to_name(e06));

    vTaskDelay(pdMS_TO_TICKS(100));

    /* Read back the registers we set, to confirm the writes actually landed. */
    uint8_t r10 = 0, r11 = 0, r13 = 0, r16 = 0, r06 = 0;
    PM1_READ(0x10, &r10); PM1_READ(0x11, &r11); PM1_READ(0x13, &r13); PM1_READ(0x16, &r16);
    PM1_READ(0x06, &r06);
    ESP_LOGI(TAG,
             "M5PM1 readback: 0x16=0x%02X 0x10=0x%02X 0x13=0x%02X 0x11=0x%02X 0x06=0x%02X "
             "(want gpio2/bit2 set in 0x10&0x11; PortA 5V = bit3 of 0x06)",
             r16, r10, r13, r11, r06);

#undef PM1_WRITE
#undef PM1_READ
#undef PM1_RETRY
    ESP_LOGI(TAG, "M5PM1 PMIC init done (addr 0x%02X on I2C SDA=%d SCL=%d)",
             BOARD_PMIC_ADDR, BOARD_PMIC_SDA, BOARD_PMIC_SCL);
}
#endif

void furi_hal_init_early(void) {
    furi_hal_cortex_init_early();

#ifdef BOARD_HAS_M5PM1
    /* M5Stick S3: bring up the M5PM1 PMIC (enables the LCD power rail) before
     * anything else, or the panel stays dark. */
    furi_hal_m5pm1_init();
#endif

#ifdef BOARD_PIN_PWR_EN
    /* Power-enable must be set early — powers CC1101, BQ27220 fuel gauge, WS2812 */
    static const GpioPin pwr_en = {.port = NULL, .pin = BOARD_PIN_PWR_EN};
    furi_hal_gpio_init_simple(&pwr_en, GpioModeOutputPushPull);
    furi_hal_gpio_write(&pwr_en, true);
    ESP_LOGI(TAG, "PWR_EN GPIO%d set HIGH", BOARD_PIN_PWR_EN);
#endif

#ifdef BOARD_PIN_NRF24_CSN
    /* T-Embed Plus shares SPI2 between CC1101 and NRF24. Drive NRF24 CSN HIGH
     * (deselected) and CE LOW (standby) at boot, before any CC1101 SPI traffic.
     * Without this, NRF24 sees CC1101 traffic and corrupts the bus, manifesting
     * as a stuck ~312 MHz reading in the Frequency Analyzer and total RX failure. */
    static const GpioPin nrf24_csn = {.port = NULL, .pin = BOARD_PIN_NRF24_CSN};
    furi_hal_gpio_init_simple(&nrf24_csn, GpioModeOutputPushPull);
    furi_hal_gpio_write(&nrf24_csn, true);
    ESP_LOGI(TAG, "NRF24_CSN GPIO%d set HIGH (deselect)", BOARD_PIN_NRF24_CSN);
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
    furi_hal_subghz_init();
    furi_hal_usb_init();
    furi_hal_light_init();
    furi_hal_display_init();
    furi_hal_speaker_init();
    furi_hal_nfc_init();
    ESP_LOGI(TAG, "Init complete");
}
