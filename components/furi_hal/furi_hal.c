#include "furi_hal.h"
#include "boards/board.h"
#include <furi_hal_gpio.h>
#include "furi_hal_uart_shared.h"
#include <esp_log.h>
#include "furi_hal_external_uart.h"
#include <nvs_flash.h>
#include <furi_hal_sd.h>

#include <driver/gpio.h>
#include <esp_rom_sys.h>

#ifdef BOARD_HEARTBEAT_DEBUG
#include <freertos/task.h>
#include <esp_system.h>
#endif

static const char* TAG = "FuriHal";

#ifdef BOARD_HEARTBEAT_DEBUG
/* Diagnostic task: proves the system is still scheduling and, once, dumps the
 * state of every FreeRTOS task. When the UI "freezes", the serial log tells us
 * immediately whether the whole system wedged (heartbeat stops) or only some
 * service is blocked (heartbeat keeps going and the dump shows which task is
 * not Ready). Only compiled in for boards that opt in. */
static void furi_hal_heartbeat_task(void* context) {
    UNUSED(context);
    uint32_t beat = 0;

    while(true) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        beat++;
        ESP_LOGI(
            TAG,
            "heartbeat=%lu heap=%u min=%u",
            (unsigned long)beat,
            (unsigned)esp_get_free_heap_size(),
            (unsigned)esp_get_minimum_free_heap_size());

        if(beat == 5) {
            TaskStatus_t statuses[32];
            UBaseType_t count = uxTaskGetSystemState(statuses, 32, NULL);
            for(UBaseType_t i = 0; i < count; i++) {
                ESP_LOGI(
                    TAG,
                    "  task %-18s state=%u prio=%u hwm=%u",
                    statuses[i].pcTaskName,
                    (unsigned)statuses[i].eCurrentState,
                    (unsigned)statuses[i].uxCurrentPriority,
                    (unsigned)statuses[i].usStackHighWaterMark);
            }
        }
    }
}
#endif

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

#if CONFIG_T_EMBED_PLUS_SD_RECOVERY
#if BOARD_PIN_SD_CS != 13 || BOARD_PIN_NRF24_CSN != 44 || BOARD_PIN_NRF24_CE != 43 || !BOARD_CC1101_SPI_SHARED
#error "Plus SD recovery requires the T-Embed CC1101 shared-SPI pin layout"
#endif
#endif

#if CONFIG_T_EMBED_PLUS_SD_RECOVERY || defined(BOARD_EARLY_SD_INIT)
    /* Preload every shared-bus select before enabling its output. A reset
     * need not reset the peripherals: keep them deselected before power-up
     * and before any SPI clock, including the CC1101 probe. */
    const GpioPin* shared_selects[] = {
        &gpio_sdcard_cs, &gpio_lcd_cs, &gpio_ext_pa4, &gpio_nrf24_cs,
#ifdef BOARD_PIN_NFC_CS
        &gpio_nfc_cs,
#endif
    };
    for(size_t i = 0; i < sizeof(shared_selects) / sizeof(shared_selects[0]); ++i) {
        furi_hal_gpio_write(shared_selects[i], true);
        furi_hal_gpio_init_simple(shared_selects[i], GpioModeOutputPushPull);
    }
    static const GpioPin nrf24_ce_idle = {.port = NULL, .pin = BOARD_PIN_NRF24_CE};
    furi_hal_gpio_write(&nrf24_ce_idle, false);
    furi_hal_gpio_init_simple(&nrf24_ce_idle, GpioModeOutputPushPull);
#endif
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
#ifdef BOARD_NFC_EARLY_PROBE
    /* Must run before the SD card and the display bring up SPI2. */
    furi_hal_nfc_early_probe();
#endif
#if CONFIG_T_EMBED_PLUS_SD_RECOVERY || defined(BOARD_EARLY_SD_INIT)
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

#ifdef BOARD_HEARTBEAT_DEBUG
    xTaskCreate(furi_hal_heartbeat_task, "hb", 4096, NULL, 6, NULL);
#endif
}
