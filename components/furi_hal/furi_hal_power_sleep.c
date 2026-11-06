/**
 * @file furi_hal_power_sleep.c
 * Defined pad states for light sleep and deep sleep, for every board.
 *
 * Rules used to derive each board's table from its header:
 *  - Outputs that drive another chip go to their inactive level: chip
 *    selects high, resets / clocks / data / LED gates low, backlight off.
 *  - Lines driven by another chip (radio GDOx, IRQs, encoder contacts) are
 *    left high-impedance; their input buffer is off, so they draw nothing.
 *  - Switch-to-ground buttons get a pull-up (no current while released).
 *  - Boards with a PWR_EN-switched peripheral rail (T-Embed) use weak pulls
 *    instead of driven highs for chip selects, so nothing back-feeds a rail
 *    that is off in deep sleep.
 *  - The deep-sleep wake pin (BOOT) and externally biased nets (BQ25896 /QON)
 *    are never touched; the battery-sense ADC pin neither.
 *  - Strapping pins GPIO45 / GPIO46 are never pulled or driven high in deep
 *    sleep (GPIO45 high would select a 1.8 V flash supply if re-sampled).
 * A later table entry for the same pin overrides an earlier one, so a board
 * header can refine the derived states with FURI_HAL_POWER_*_EXTRA_PINS.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "furi_hal_power_sleep.h"
#include "furi_hal_resources.h"
#include "boards/board.h"

#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_rom_sys.h>
#include <esp_sleep.h>
#include <soc/reset_reasons.h>
#include <soc/soc_caps.h>

#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#define TAG "FuriHalPowerSleep"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

#if defined(BOARD_PIN_PWR_EN) && (BOARD_PIN_PWR_EN >= 0) && (BOARD_PIN_PWR_EN < 64)
#define FURI_HAL_POWER_HAS_PWR_RAIL 1
#else
#define FURI_HAL_POWER_HAS_PWR_RAIL 0
#endif

#if FURI_HAL_POWER_HAS_PWR_RAIL
/* Peripherals may be unpowered in deep sleep: weak pulls only. */
#define DSLP_CS_IDLE     FuriHalPowerPinPullUp
#define DSLP_RADIO_CS    FuriHalPowerPinPullDown /* CC1101 sits on the switched rail */
#define DSLP_SHARED_MISO FuriHalPowerPinExternal
#else
#define DSLP_CS_IDLE     FuriHalPowerPinHigh
#define DSLP_RADIO_CS    FuriHalPowerPinHigh
#define DSLP_SHARED_MISO FuriHalPowerPinPullUp
#endif

#if BOARD_LCD_BL_ACTIVE_LOW
#define DSLP_BL_OFF FuriHalPowerPinHigh
#define LS_BL_OFF   FuriHalPowerLsPinPullUp
#else
#define DSLP_BL_OFF FuriHalPowerPinLow
#define LS_BL_OFF   FuriHalPowerLsPinPullDown
#endif

#ifndef FURI_HAL_POWER_DEEP_SLEEP_EXTRA_PINS
#define FURI_HAL_POWER_DEEP_SLEEP_EXTRA_PINS
#endif
#ifndef FURI_HAL_POWER_LIGHT_SLEEP_EXTRA_PINS
#define FURI_HAL_POWER_LIGHT_SLEEP_EXTRA_PINS
#endif

typedef struct {
    int pin;
    FuriHalPowerPinMode mode;
} FuriHalPowerPinEntry;

typedef struct {
    int pin;
    FuriHalPowerLsPinMode mode;
} FuriHalPowerLsPinEntry;

/* ---------------------------------------------------------------------------
 * Deep-sleep table
 * ------------------------------------------------------------------------- */
static const FuriHalPowerPinEntry furi_hal_power_dslp_pins[] = {
#ifdef BOARD_PIN_PWR_EN
    {BOARD_PIN_PWR_EN, FuriHalPowerPinLow},
#endif
#ifdef BOARD_PIN_WS2812_DATA
    {BOARD_PIN_WS2812_DATA, FuriHalPowerPinLow},
#endif
#ifdef BOARD_PIN_LCD_BL
    {BOARD_PIN_LCD_BL, DSLP_BL_OFF},
#endif
#ifdef BOARD_PIN_LCD_RST
    {BOARD_PIN_LCD_RST, FuriHalPowerPinLow},
#endif
#ifdef BOARD_PIN_LCD_DC
    {BOARD_PIN_LCD_DC, FuriHalPowerPinLow},
#endif
#ifdef BOARD_PIN_LCD_MOSI
    {BOARD_PIN_LCD_MOSI, FuriHalPowerPinLow},
#endif
#ifdef BOARD_PIN_LCD_SCLK
    {BOARD_PIN_LCD_SCLK, FuriHalPowerPinLow},
#endif
#ifdef BOARD_PIN_LCD_CS
    {BOARD_PIN_LCD_CS, DSLP_CS_IDLE},
#endif
#ifdef BOARD_PIN_DISPLAY_RST
    {BOARD_PIN_DISPLAY_RST, FuriHalPowerPinLow},
#endif
#ifdef BOARD_PIN_DISPLAY_SDA
    {BOARD_PIN_DISPLAY_SDA, FuriHalPowerPinPullUp},
    {BOARD_PIN_DISPLAY_SCL, FuriHalPowerPinPullUp},
#endif
#ifdef BOARD_PIN_SD_CS
    {BOARD_PIN_SD_CS, DSLP_CS_IDLE},
#endif
#ifdef BOARD_PIN_SD_MISO
    {BOARD_PIN_SD_MISO, DSLP_SHARED_MISO},
#endif
#ifdef BOARD_SD_BUS_NOT_SHARED
    {BOARD_PIN_SD_MISO2, FuriHalPowerPinPullUp},
    {BOARD_PIN_SD_MOSI, FuriHalPowerPinLow},
    {BOARD_PIN_SD_SCK, FuriHalPowerPinLow},
#endif
#ifdef BOARD_PIN_CC1101_CSN
    {BOARD_PIN_CC1101_CSN, DSLP_RADIO_CS},
    {BOARD_PIN_CC1101_SCK, FuriHalPowerPinLow},
    {BOARD_PIN_CC1101_MOSI, FuriHalPowerPinLow},
    {BOARD_PIN_CC1101_MISO, DSLP_SHARED_MISO},
    {BOARD_PIN_CC1101_GDO0, FuriHalPowerPinExternal},
#endif
#ifdef BOARD_PIN_CC1101_GDO2
    {BOARD_PIN_CC1101_GDO2, FuriHalPowerPinExternal},
#endif
#ifdef BOARD_PIN_CC1101_SW0
    {BOARD_PIN_CC1101_SW0, FuriHalPowerPinLow},
    {BOARD_PIN_CC1101_SW1, FuriHalPowerPinLow},
#endif
#if defined(BOARD_PIN_RFID_TX) && defined(BOARD_PIN_RFID_RX)
    /* UART lines idle high. Listed before the NRF24 so its CE/CSN wins where
     * the pads are shared (T-Embed GPIO43/44). */
    {BOARD_PIN_RFID_TX, FuriHalPowerPinPullUp},
    {BOARD_PIN_RFID_RX, FuriHalPowerPinPullUp},
#endif
#ifdef BOARD_PIN_NRF24_CSN
    {BOARD_PIN_NRF24_CSN, DSLP_CS_IDLE},
#endif
#ifdef BOARD_PIN_NRF24_CE
    {BOARD_PIN_NRF24_CE, FuriHalPowerPinLow},
#endif
#if CONFIG_T_EMBED_EXTERNAL_UART && defined(BOARD_EXTERNAL_UART_TX)
    {BOARD_EXTERNAL_UART_TX, FuriHalPowerPinPullUp},
    {BOARD_EXTERNAL_UART_RX, FuriHalPowerPinPullUp},
#endif
#ifdef BOARD_PIN_IR_TX
    {BOARD_PIN_IR_TX, FuriHalPowerPinLow},
#endif
#ifdef BOARD_PIN_IR_RX
    {BOARD_PIN_IR_RX, FuriHalPowerPinPullUp},
#endif
#if defined(BOARD_PIN_QWIIC_SDA) && defined(BOARD_PIN_QWIIC_SCL)
    /* I2C idles high through pull-ups, never push-pull. */
    {BOARD_PIN_QWIIC_SDA, FuriHalPowerPinPullUp},
    {BOARD_PIN_QWIIC_SCL, FuriHalPowerPinPullUp},
#endif
#if defined(BOARD_PIN_NFC_SDA) && defined(BOARD_PIN_NFC_SCL)
    {BOARD_PIN_NFC_SDA, FuriHalPowerPinPullUp},
    {BOARD_PIN_NFC_SCL, FuriHalPowerPinPullUp},
#endif
#ifdef BOARD_PIN_NFC_RST
    {BOARD_PIN_NFC_RST, FuriHalPowerPinLow}, /* PN532 hard power-down */
#endif
#ifdef BOARD_PIN_NFC_IRQ
    {BOARD_PIN_NFC_IRQ, FuriHalPowerPinExternal},
#endif
#ifdef BOARD_PIN_TOUCH_RST
    {BOARD_PIN_TOUCH_RST, FuriHalPowerPinLow},
    {BOARD_PIN_TOUCH_INT, FuriHalPowerPinExternal},
    {BOARD_PIN_TOUCH_SDA, FuriHalPowerPinPullUp},
    {BOARD_PIN_TOUCH_SCL, FuriHalPowerPinPullUp},
#endif
#ifdef BOARD_PIN_IMU_INT1
    {BOARD_PIN_IMU_INT1, FuriHalPowerPinExternal},
    {BOARD_PIN_IMU_INT2, FuriHalPowerPinExternal},
#endif
#if defined(BOARD_HAS_SPEAKER) && BOARD_HAS_SPEAKER
    {BOARD_PIN_SPEAKER_BCLK, FuriHalPowerPinLow},
    {BOARD_PIN_SPEAKER_WCLK, FuriHalPowerPinLow},
    {BOARD_PIN_SPEAKER_DOUT, FuriHalPowerPinLow},
#endif
#if defined(BOARD_HAS_MIC) && BOARD_HAS_MIC && defined(BOARD_PIN_MIC_CLK)
    {BOARD_PIN_MIC_CLK, FuriHalPowerPinLow},
    {BOARD_PIN_MIC_DATA, FuriHalPowerPinExternal},
#endif
#ifdef BOARD_PIN_BUZZER
    {BOARD_PIN_BUZZER, FuriHalPowerPinLow},
#endif
#ifdef BOARD_PIN_EXT_GPIO_1
    /* Unused header pins: defined low through a pull-down, never driven. */
    {BOARD_PIN_EXT_GPIO_1, FuriHalPowerPinPullDown},
    {BOARD_PIN_EXT_GPIO_2, FuriHalPowerPinPullDown},
    {BOARD_PIN_EXT_GPIO_3, FuriHalPowerPinPullDown},
    {BOARD_PIN_EXT_GPIO_4, FuriHalPowerPinPullDown},
    {BOARD_PIN_EXT_GPIO_5, FuriHalPowerPinPullDown},
#endif
#ifdef BOARD_PIN_ENCODER_A
    /* Encoder contacts may rest closed: high-Z, so no pull leaks through them. */
    {BOARD_PIN_ENCODER_A, FuriHalPowerPinExternal},
    {BOARD_PIN_ENCODER_B, FuriHalPowerPinExternal},
#endif
#ifdef BOARD_PIN_BUTTON_KEY
    {BOARD_PIN_BUTTON_KEY, FuriHalPowerPinPullUp},
#endif
#ifdef BOARD_PIN_ENCODER_BTN
    {BOARD_PIN_ENCODER_BTN, FuriHalPowerPinPullUp},
#endif
#ifdef BOARD_PIN_BTN_UP
    {BOARD_PIN_BTN_UP, FuriHalPowerPinPullUp},
    {BOARD_PIN_BTN_DOWN, FuriHalPowerPinPullUp},
    {BOARD_PIN_BTN_LEFT, FuriHalPowerPinPullUp},
    {BOARD_PIN_BTN_RIGHT, FuriHalPowerPinPullUp},
    {BOARD_PIN_BTN_BACK, FuriHalPowerPinPullUp},
#endif
#ifdef BOARD_PIN_BQ25896_QON
    /* /QON is biased by the BQ25896: never drive or pull it. */
    {BOARD_PIN_BQ25896_QON, FuriHalPowerPinExternal},
#endif
    FURI_HAL_POWER_DEEP_SLEEP_EXTRA_PINS
    /* Wake source, last so nothing overrides it. */
    {BOARD_PIN_BUTTON_BOOT, FuriHalPowerPinExternal},
};

/* GPIOs excluded from the "unmanaged" audit: flash/PSRAM and USB PHY. */
#if CONFIG_IDF_TARGET_ESP32S3
#define FURI_HAL_POWER_AUDIT_SKIP_MASK \
    ((1ULL << 19) | (1ULL << 20) | (((1ULL << 12) - 1ULL) << 26))
#elif CONFIG_IDF_TARGET_ESP32C6
#define FURI_HAL_POWER_AUDIT_SKIP_MASK \
    ((1ULL << 12) | (1ULL << 13) | (((1ULL << 7) - 1ULL) << 24))
#else
#define FURI_HAL_POWER_AUDIT_SKIP_MASK 0ULL
#endif

#if SOC_GPIO_SUPPORT_HOLD_IO_IN_DSLP && !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
#define FURI_HAL_POWER_GLOBAL_DSLP_HOLD 1 /* ESP32-S3 */
#else
#define FURI_HAL_POWER_GLOBAL_DSLP_HOLD 0 /* ESP32-C6: per-pad hold */
#endif

static bool furi_hal_power_pin_valid(int pin) {
    return pin >= 0 && pin < GPIO_NUM_MAX && GPIO_IS_VALID_GPIO(pin);
}

/* Wake pin and battery divider: never driven, pulled or held. */
static bool furi_hal_power_pin_reserved(int pin) {
    if(pin == (int)BOARD_PIN_BUTTON_BOOT) return true;
    if(pin == (int)gpio_battery_sense.pin) return true;
    return false;
}

/* The effective entry for a pin is its last one in the table. */
static bool furi_hal_power_dslp_is_last(size_t index) {
    for(size_t j = index + 1; j < COUNT(furi_hal_power_dslp_pins); j++) {
        if(furi_hal_power_dslp_pins[j].pin == furi_hal_power_dslp_pins[index].pin) return false;
    }
    return true;
}

static FuriHalPowerPinMode furi_hal_power_dslp_mode(size_t index) {
    const int pin = furi_hal_power_dslp_pins[index].pin;
    FuriHalPowerPinMode mode = furi_hal_power_dslp_pins[index].mode;
    if((pin == 45 || pin == 46) &&
       (mode == FuriHalPowerPinHigh || mode == FuriHalPowerPinPullUp)) {
        mode = FuriHalPowerPinExternal; /* strapping pin: never high */
    }
    return mode;
}

static bool furi_hal_power_pin_is_managed(int pin) {
    if(furi_hal_power_pin_reserved(pin)) return true;
    for(size_t i = 0; i < COUNT(furi_hal_power_dslp_pins); i++) {
        if(furi_hal_power_dslp_pins[i].pin == pin) return true;
    }
    return false;
}

static void furi_hal_power_digital_set(gpio_num_t pin, FuriHalPowerPinMode mode) {
    switch(mode) {
    case FuriHalPowerPinLow:
    case FuriHalPowerPinHigh:
        gpio_set_level(pin, mode == FuriHalPowerPinHigh ? 1 : 0); /* level first: no glitch */
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_pull_mode(pin, GPIO_FLOATING);
        break;
    case FuriHalPowerPinPullUp:
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
        break;
    case FuriHalPowerPinPullDown:
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
        break;
    default:
        break;
    }
}

static bool furi_hal_power_holds_released = false;

void furi_hal_power_sleep_release_holds(void) {
    if(furi_hal_power_holds_released) return;
    furi_hal_power_holds_released = true;

    /* After a deep-sleep wake the pads are still latched in their sleep
     * state. Preload the same state into the run-mode registers before the
     * latch is opened, so no pad floats between "released" and "configured
     * by its driver". On any other boot just make sure nothing stays held. */
    const bool from_deep_sleep =
        esp_rom_get_reset_reason(0) == RESET_REASON_CORE_DEEP_SLEEP;

#if FURI_HAL_POWER_GLOBAL_DSLP_HOLD
    gpio_deep_sleep_hold_dis();
#endif

    for(size_t i = 0; i < COUNT(furi_hal_power_dslp_pins); i++) {
        const int pin = furi_hal_power_dslp_pins[i].pin;
        if(!furi_hal_power_pin_valid(pin) || furi_hal_power_pin_reserved(pin)) continue;
        if(!furi_hal_power_dslp_is_last(i)) continue;
        const FuriHalPowerPinMode mode = furi_hal_power_dslp_mode(i);
        if(mode == FuriHalPowerPinExternal) continue;

        if(from_deep_sleep) furi_hal_power_digital_set((gpio_num_t)pin, mode);
#if FURI_HAL_POWER_GLOBAL_DSLP_HOLD
        if(rtc_gpio_is_valid_gpio((gpio_num_t)pin)) {
            rtc_gpio_hold_dis((gpio_num_t)pin);
            rtc_gpio_deinit((gpio_num_t)pin); /* mux back to the digital function */
        } else {
            gpio_hold_dis((gpio_num_t)pin);
        }
#else
        gpio_hold_dis((gpio_num_t)pin);
#endif
    }

#if SOC_PM_SUPPORT_EXT0_WAKEUP
    /* EXT0 left the wake pin in RTC mode. */
    if(rtc_gpio_is_valid_gpio((gpio_num_t)BOARD_PIN_BUTTON_BOOT)) {
        rtc_gpio_deinit((gpio_num_t)BOARD_PIN_BUTTON_BOOT);
    }
#endif
}

/* Runs before app_main, so every hold is released before any HAL driver
 * touches the pins. No logging this early. */
__attribute__((constructor(101))) static void furi_hal_power_sleep_early_release(void) {
    furi_hal_power_sleep_release_holds();
}

void furi_hal_power_sleep_audit_pins(void) {
    char list[160] = {0};
    size_t len = 0;
    bool truncated = false;

    for(int n = 0; n < (int)GPIO_NUM_MAX; n++) {
        if(!GPIO_IS_VALID_GPIO(n)) continue;
        if((FURI_HAL_POWER_AUDIT_SKIP_MASK >> n) & 1ULL) continue;
        if(furi_hal_power_pin_is_managed(n)) continue;
        const int w = snprintf(list + len, sizeof(list) - len, "%d ", n);
        if(w < 0 || (size_t)w >= sizeof(list) - len) {
            truncated = true;
            break;
        }
        len += (size_t)w;
    }

    if(len > 0) {
        ESP_LOGI(TAG, "GPIOs without a sleep state (unconnected?): %s%s", list, truncated ? "..." : "");
    } else {
        ESP_LOGI(TAG, "GPIO audit: every pin has a sleep state");
    }
}

bool furi_hal_power_sleep_isolate_pins(void) {
    bool need_digital_hold = false;
    unsigned held = 0;

    for(size_t i = 0; i < COUNT(furi_hal_power_dslp_pins); i++) {
        const int pin_num = furi_hal_power_dslp_pins[i].pin;
        if(!furi_hal_power_pin_valid(pin_num) || furi_hal_power_pin_reserved(pin_num)) continue;
        if(!furi_hal_power_dslp_is_last(i)) continue;
        const FuriHalPowerPinMode mode = furi_hal_power_dslp_mode(i);
        if(mode == FuriHalPowerPinExternal) continue;
        const gpio_num_t pin = (gpio_num_t)pin_num;

#if FURI_HAL_POWER_GLOBAL_DSLP_HOLD
        if(rtc_gpio_is_valid_gpio(pin)) {
            /* RTC pad: per-pad hold, no global digital isolation needed. */
            switch(mode) {
            case FuriHalPowerPinLow:
            case FuriHalPowerPinHigh:
                rtc_gpio_set_level(pin, mode == FuriHalPowerPinHigh ? 1 : 0);
                rtc_gpio_init(pin);
                rtc_gpio_set_direction(pin, RTC_GPIO_MODE_OUTPUT_ONLY);
                rtc_gpio_pullup_dis(pin);
                rtc_gpio_pulldown_dis(pin);
                break;
            case FuriHalPowerPinPullUp:
                rtc_gpio_init(pin);
                rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
                rtc_gpio_pulldown_dis(pin);
                rtc_gpio_pullup_en(pin);
                break;
            case FuriHalPowerPinPullDown:
                rtc_gpio_init(pin);
                rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
                rtc_gpio_pullup_dis(pin);
                rtc_gpio_pulldown_en(pin);
                break;
            default:
                continue;
            }
            rtc_gpio_hold_en(pin);
        } else {
            /* Digital pad: needs gpio_deep_sleep_hold_en() from an
             * internal-RAM stack, see furi_hal_power_sleep_start_deep_sleep(). */
            furi_hal_power_digital_set(pin, mode);
            gpio_hold_en(pin);
            need_digital_hold = true;
        }
#else
        furi_hal_power_digital_set(pin, mode);
        gpio_hold_en(pin);
#endif
        held++;
    }

    ESP_LOGI(TAG, "Deep-sleep pin states latched on %u pins (digital hold: %d)", held, need_digital_hold);
    return need_digital_hold;
}

static volatile bool furi_hal_power_dslp_digital_hold = false;

/* TCB and stack of the deep-sleep task are static: they live in .bss, which is
 * always internal RAM (only EXT_RAM_BSS_ATTR objects go to PSRAM), and creating
 * the task cannot fail for lack of internal heap. A dynamic allocation could,
 * and the only alternative was entering deep sleep without the global digital
 * pad hold, leaving every held digital pad floating while asleep. */
#define FURI_HAL_POWER_DSLEEP_STACK_BYTES 4096U
static StaticTask_t furi_hal_power_dsleep_tcb;
static StackType_t furi_hal_power_dsleep_stack[FURI_HAL_POWER_DSLEEP_STACK_BYTES /
                                               sizeof(StackType_t)] __attribute__((aligned(16)));
/* The TCB/stack are single-use: only the first caller creates the task. */
static bool furi_hal_power_dsleep_started = false;

/* Runs on an internal-RAM stack: with digital pads held,
 * esp_deep_sleep_start() asserts unless the calling stack is internal, and
 * the power-service stack lives in PSRAM. Never returns. */
static void furi_hal_power_deep_sleep_task(void* arg) {
    (void)arg;
#if FURI_HAL_POWER_GLOBAL_DSLP_HOLD
    if(furi_hal_power_dslp_digital_hold) gpio_deep_sleep_hold_en();
#endif
    esp_deep_sleep_start();
    for(;;) {
    }
}

void furi_hal_power_sleep_start_deep_sleep(bool digital_hold) {
    /* A second concurrent caller must neither re-create the task on the same
     * TCB nor overwrite the hold flag the running task reads: it just parks
     * until the chip goes down. */
    if(!__atomic_exchange_n(&furi_hal_power_dsleep_started, true, __ATOMIC_ACQ_REL)) {
        assert(
            esp_ptr_internal(furi_hal_power_dsleep_stack) &&
            esp_ptr_internal(&furi_hal_power_dsleep_tcb));
        furi_hal_power_dslp_digital_hold = digital_hold;
        /* Static creation returns NULL only for invalid arguments. */
        const TaskHandle_t handle = xTaskCreateStaticPinnedToCore(
            furi_hal_power_deep_sleep_task,
            "dsleep",
            sizeof(furi_hal_power_dsleep_stack) / sizeof(StackType_t),
            NULL,
            configMAX_PRIORITIES - 1,
            furi_hal_power_dsleep_stack,
            &furi_hal_power_dsleep_tcb,
            xPortGetCoreID());
        assert(handle != NULL);
        (void)handle;
    }
    for(;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

#if CONFIG_PM_ENABLE
/* ---------------------------------------------------------------------------
 * Light-sleep table
 *
 * With CONFIG_PM_SLP_DISABLE_GPIO (selected by ESP_SLEEP_GPIO_RESET_WORKAROUND
 * on the S3) every pad is isolated during light sleep: input, output and
 * pulls off, so external inputs wired to them float. This table gives each
 * connected pad a sleep state instead. Pads that are GPIO wake sources keep
 * their run-mode configuration while waking is enabled (gpio_wakeup_enable
 * turns their sleep selection off), so listing a button here is harmless.
 * ------------------------------------------------------------------------- */
static const FuriHalPowerLsPinEntry furi_hal_power_ls_pins[] = {
#ifdef BOARD_PIN_PWR_EN
    {BOARD_PIN_PWR_EN, FuriHalPowerLsPinKeepOutput},
#endif
#ifdef BOARD_PIN_WS2812_DATA
    /* Held low: no stray edge can latch a colour into the (black) LEDs. */
    {BOARD_PIN_WS2812_DATA, FuriHalPowerLsPinPullDown},
#endif
#ifdef BOARD_PIN_LCD_BL
    {BOARD_PIN_LCD_BL, LS_BL_OFF},
#endif
#ifdef BOARD_PIN_LCD_RST
    /* Must stay high: a reset would drop the panel setup (wake only sends SLPOUT). */
    {BOARD_PIN_LCD_RST, FuriHalPowerLsPinPullUp},
#endif
#ifdef BOARD_PIN_LCD_CS
    {BOARD_PIN_LCD_CS, FuriHalPowerLsPinPullUp},
#endif
#ifdef BOARD_PIN_LCD_DC
    {BOARD_PIN_LCD_DC, FuriHalPowerLsPinPullDown},
#endif
#ifdef BOARD_PIN_LCD_MOSI
    {BOARD_PIN_LCD_MOSI, FuriHalPowerLsPinPullDown},
#endif
#ifdef BOARD_PIN_LCD_SCLK
    {BOARD_PIN_LCD_SCLK, FuriHalPowerLsPinPullDown},
#endif
#ifdef BOARD_PIN_SD_CS
    {BOARD_PIN_SD_CS, FuriHalPowerLsPinPullUp},
#endif
#ifdef BOARD_PIN_SD_MISO
    {BOARD_PIN_SD_MISO, FuriHalPowerLsPinPullUp},
#endif
#ifdef BOARD_SD_BUS_NOT_SHARED
    {BOARD_PIN_SD_MISO2, FuriHalPowerLsPinPullUp},
    {BOARD_PIN_SD_MOSI, FuriHalPowerLsPinPullDown},
    {BOARD_PIN_SD_SCK, FuriHalPowerLsPinPullDown},
#endif
#ifdef BOARD_PIN_CC1101_CSN
    /* CSn high keeps the CC1101 in its SLEEP state. */
    {BOARD_PIN_CC1101_CSN, FuriHalPowerLsPinPullUp},
    {BOARD_PIN_CC1101_SCK, FuriHalPowerLsPinPullDown},
    {BOARD_PIN_CC1101_MOSI, FuriHalPowerLsPinPullDown},
    {BOARD_PIN_CC1101_MISO, FuriHalPowerLsPinPullUp},
    {BOARD_PIN_CC1101_GDO0, FuriHalPowerLsPinNoPull},
#endif
#ifdef BOARD_PIN_CC1101_GDO2
    {BOARD_PIN_CC1101_GDO2, FuriHalPowerLsPinNoPull},
#endif
#ifdef BOARD_PIN_CC1101_SW0
    {BOARD_PIN_CC1101_SW0, FuriHalPowerLsPinKeepOutput},
    {BOARD_PIN_CC1101_SW1, FuriHalPowerLsPinKeepOutput},
#endif
#if defined(BOARD_PIN_RFID_TX) && defined(BOARD_PIN_RFID_RX)
    {BOARD_PIN_RFID_TX, FuriHalPowerLsPinPullUp},
    {BOARD_PIN_RFID_RX, FuriHalPowerLsPinPullUp},
#endif
#ifdef BOARD_PIN_NRF24_CSN
    {BOARD_PIN_NRF24_CSN, FuriHalPowerLsPinPullUp},
#endif
#ifdef BOARD_PIN_NRF24_CE
    {BOARD_PIN_NRF24_CE, FuriHalPowerLsPinPullDown},
#endif
#if CONFIG_T_EMBED_EXTERNAL_UART && defined(BOARD_EXTERNAL_UART_TX)
    {BOARD_EXTERNAL_UART_TX, FuriHalPowerLsPinPullUp},
    {BOARD_EXTERNAL_UART_RX, FuriHalPowerLsPinPullUp},
#endif
#ifdef BOARD_PIN_IR_TX
    {BOARD_PIN_IR_TX, FuriHalPowerLsPinPullDown},
#endif
#ifdef BOARD_PIN_IR_RX
    {BOARD_PIN_IR_RX, FuriHalPowerLsPinPullUp},
#endif
#if defined(BOARD_PIN_QWIIC_SDA) && defined(BOARD_PIN_QWIIC_SCL)
    {BOARD_PIN_QWIIC_SDA, FuriHalPowerLsPinPullUp},
    {BOARD_PIN_QWIIC_SCL, FuriHalPowerLsPinPullUp},
#endif
#if defined(BOARD_PIN_NFC_SDA) && defined(BOARD_PIN_NFC_SCL)
    {BOARD_PIN_NFC_SDA, FuriHalPowerLsPinPullUp},
    {BOARD_PIN_NFC_SCL, FuriHalPowerLsPinPullUp},
#endif
#ifdef BOARD_PIN_NFC_RST
    /* Keep the PN532 running across light sleep. */
    {BOARD_PIN_NFC_RST, FuriHalPowerLsPinPullUp},
#endif
#ifdef BOARD_PIN_NFC_IRQ
    {BOARD_PIN_NFC_IRQ, FuriHalPowerLsPinNoPull},
#endif
#ifdef BOARD_PIN_DISPLAY_SDA
    {BOARD_PIN_DISPLAY_SDA, FuriHalPowerLsPinPullUp},
    {BOARD_PIN_DISPLAY_SCL, FuriHalPowerLsPinPullUp},
#endif
#if defined(BOARD_HAS_SPEAKER) && BOARD_HAS_SPEAKER
    {BOARD_PIN_SPEAKER_BCLK, FuriHalPowerLsPinPullDown},
    {BOARD_PIN_SPEAKER_WCLK, FuriHalPowerLsPinPullDown},
    {BOARD_PIN_SPEAKER_DOUT, FuriHalPowerLsPinPullDown},
#endif
#if defined(BOARD_HAS_MIC) && BOARD_HAS_MIC && defined(BOARD_PIN_MIC_CLK)
    {BOARD_PIN_MIC_CLK, FuriHalPowerLsPinPullDown},
    {BOARD_PIN_MIC_DATA, FuriHalPowerLsPinNoPull},
#endif
#ifdef BOARD_PIN_BUZZER
    {BOARD_PIN_BUZZER, FuriHalPowerLsPinPullDown},
#endif
#ifdef BOARD_PIN_EXT_GPIO_1
    {BOARD_PIN_EXT_GPIO_1, FuriHalPowerLsPinPullDown},
    {BOARD_PIN_EXT_GPIO_2, FuriHalPowerLsPinPullDown},
    {BOARD_PIN_EXT_GPIO_3, FuriHalPowerLsPinPullDown},
    {BOARD_PIN_EXT_GPIO_4, FuriHalPowerLsPinPullDown},
    {BOARD_PIN_EXT_GPIO_5, FuriHalPowerLsPinPullDown},
#endif
#ifdef BOARD_PIN_ENCODER_A
    {BOARD_PIN_ENCODER_A, FuriHalPowerLsPinNoPull},
    {BOARD_PIN_ENCODER_B, FuriHalPowerLsPinNoPull},
#endif
#ifdef BOARD_PIN_BUTTON_KEY
    {BOARD_PIN_BUTTON_KEY, FuriHalPowerLsPinPullUp},
#endif
#ifdef BOARD_PIN_ENCODER_BTN
    {BOARD_PIN_ENCODER_BTN, FuriHalPowerLsPinPullUp},
#endif
#ifdef BOARD_PIN_BTN_UP
    {BOARD_PIN_BTN_UP, FuriHalPowerLsPinPullUp},
    {BOARD_PIN_BTN_DOWN, FuriHalPowerLsPinPullUp},
    {BOARD_PIN_BTN_LEFT, FuriHalPowerLsPinPullUp},
    {BOARD_PIN_BTN_RIGHT, FuriHalPowerLsPinPullUp},
    {BOARD_PIN_BTN_BACK, FuriHalPowerLsPinPullUp},
#endif
#ifdef BOARD_PIN_BQ25896_QON
    {BOARD_PIN_BQ25896_QON, FuriHalPowerLsPinNoPull},
#endif
    FURI_HAL_POWER_LIGHT_SLEEP_EXTRA_PINS
    {BOARD_PIN_BUTTON_BOOT, FuriHalPowerLsPinPullUp},
};

static bool furi_hal_power_ls_is_last(size_t index) {
    for(size_t j = index + 1; j < COUNT(furi_hal_power_ls_pins); j++) {
        if(furi_hal_power_ls_pins[j].pin == furi_hal_power_ls_pins[index].pin) return false;
    }
    return true;
}

void furi_hal_power_sleep_apply_light_sleep_pins(void) {
    static bool logged = false;
    unsigned applied = 0;
    for(size_t i = 0; i < COUNT(furi_hal_power_ls_pins); i++) {
        const int pin_num = furi_hal_power_ls_pins[i].pin;
        if(!furi_hal_power_pin_valid(pin_num)) continue;
        if(pin_num == (int)gpio_battery_sense.pin) continue;
        if(!furi_hal_power_ls_is_last(i)) continue;
        const gpio_num_t pin = (gpio_num_t)pin_num;
        switch(furi_hal_power_ls_pins[i].mode) {
        case FuriHalPowerLsPinPullUp:
            gpio_sleep_set_direction(pin, GPIO_MODE_DISABLE);
            gpio_sleep_set_pull_mode(pin, GPIO_PULLUP_ONLY);
            break;
        case FuriHalPowerLsPinPullDown:
            gpio_sleep_set_direction(pin, GPIO_MODE_DISABLE);
            gpio_sleep_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
            break;
        case FuriHalPowerLsPinNoPull:
            gpio_sleep_set_direction(pin, GPIO_MODE_DISABLE);
            gpio_sleep_set_pull_mode(pin, GPIO_FLOATING);
            break;
        case FuriHalPowerLsPinKeepOutput:
            gpio_sleep_set_direction(pin, GPIO_MODE_OUTPUT);
            gpio_sleep_set_pull_mode(pin, GPIO_FLOATING);
            break;
        }
        /* Re-applied on every gate opening in case a driver changed the
         * selection meanwhile. Wake pins are armed after this call. */
        gpio_sleep_sel_en(pin);
        applied++;
    }
    if(!logged) {
        logged = true;
        ESP_LOGI(TAG, "Light-sleep pad states configured for %u pins", applied);
    }
}

void furi_hal_power_sleep_reset_light_sleep_pins(void) {
    /* Same as esp_sleep_config_gpio_isolate() does for every pad at boot. */
    for(size_t i = 0; i < COUNT(furi_hal_power_ls_pins); i++) {
        const int pin_num = furi_hal_power_ls_pins[i].pin;
        if(!furi_hal_power_pin_valid(pin_num)) continue;
        if(pin_num == (int)gpio_battery_sense.pin) continue;
        gpio_sleep_set_direction((gpio_num_t)pin_num, GPIO_MODE_DISABLE);
        gpio_sleep_set_pull_mode((gpio_num_t)pin_num, GPIO_FLOATING);
    }
}
#endif
