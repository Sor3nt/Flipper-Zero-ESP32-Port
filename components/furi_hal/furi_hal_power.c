#include "furi_hal_power.h"
#include "furi_hal_bq27220.h"
#include "furi_hal_bq25896.h"
#include "furi_hal_display.h"
#include "furi_hal_light.h"
#include "furi_hal_speaker.h"
#include "furi_hal_bt.h"
#include "furi_hal_subghz.h"
#include "furi_hal_nfc.h"
#include "boards/board.h"

#include <math.h>
#include <stdio.h>
#include <limits.h>

#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_sleep.h>
#if CONFIG_PM_ENABLE
#include <esp_pm.h>
#endif
#include <soc/soc_caps.h>
#include <soc/gpio_periph.h>
#include <driver/i2c.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_attr.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

#include <furi.h>

#include "furi_hal_resources.h"

#define TAG "FuriHalPower"

/* Battery voltage divider on a GPIO ADC. Only boards that wire one (smoochie)
 * set it; boards with a fuel gauge (paradoxt: BQ27220) never touch the ADC. */
#ifndef BOARD_HAS_BATTERY_ADC
#if defined(BOARD_PIN_BATTERY_ADC) && (BOARD_PIN_BATTERY_ADC != UINT16_MAX)
#define BOARD_HAS_BATTERY_ADC 1
#else
#define BOARD_HAS_BATTERY_ADC 0
#endif
#endif

#if !defined(BOARD_HAS_BQ27220) || BOARD_HAS_BQ27220
#define FURI_HAL_POWER_HAS_GAUGE 1
#else
#define FURI_HAL_POWER_HAS_GAUGE 0
#endif

#if defined(BOARD_BATTERY_ADC_PRIMARY) && BOARD_BATTERY_ADC_PRIMARY
#define FURI_HAL_POWER_ADC_PRIMARY 1
#else
#define FURI_HAL_POWER_ADC_PRIMARY 0
#endif

/* VBUS is read from the charger at most this often while awake. The light
 * sleep entry and exit refresh it explicitly, so this only bounds how fast an
 * unplug is noticed by battery protection and the idle logic. */
#define FURI_HAL_POWER_VBUS_CACHE_MS (5000U)
/* Repeated diagnostics (e.g. charger VBAT without a cell) at most this often. */
#define FURI_HAL_POWER_LOG_INTERVAL_MS (60000U)

/* Battery protection (single Li-ion cell). Warning opens the power-off dialog,
 * critical disconnects the battery immediately. Both need consecutive samples
 * about a second apart so a load transient (TX burst, backlight) cannot trip
 * them, and clear only above the hysteresis level. */
#define FURI_HAL_POWER_LOW_BATTERY_CLEAR_V      (3.40f)
#define FURI_HAL_POWER_CRITICAL_BATTERY_V       (3.25f)
#define FURI_HAL_POWER_CRITICAL_BATTERY_CLEAR_V (3.35f)
#define FURI_HAL_POWER_PROTECTION_SAMPLES       (3U)
#define FURI_HAL_POWER_PROTECTION_INTERVAL_MS   (900U)

/* Default interval for the optional deep-sleep USB-removal check. */
#ifndef FURI_HAL_POWER_USB_DSLP_CHECK_S
#define FURI_HAL_POWER_USB_DSLP_CHECK_S (600U)
#endif
#define FURI_HAL_POWER_USB_DSLP_MIN_CHECK_S (60U)
#define FURI_HAL_POWER_USB_DSLP_MAX_CHECK_S (1200U)
#define FURI_HAL_POWER_USB_DSLP_MAGIC (0x55B5D5A1UL)
RTC_DATA_ATTR static uint32_t furi_hal_power_usb_dslp_magic = 0;
RTC_DATA_ATTR static uint32_t furi_hal_power_usb_dslp_interval_s = 0;
static bool furi_hal_power_usb_dslp_check_enabled = false;
static uint32_t furi_hal_power_usb_dslp_check_interval_s = FURI_HAL_POWER_USB_DSLP_CHECK_S;

static volatile bool furi_hal_power_vbus_valid = false;
static volatile bool furi_hal_power_vbus_present = true;
static volatile uint32_t furi_hal_power_vbus_tick = 0;
static volatile bool furi_hal_power_charger_adc_enabled = true;
/* Tick of the last (successful) switch-off, so a stuck-off ADC can be told from the
 * few milliseconds between switching it off and light sleep actually being granted. */
static volatile uint32_t furi_hal_power_charger_adc_off_tick = 0;
/* An ADC that is still off this long after being switched off, while awake, is stuck. */
#define FURI_HAL_POWER_CHARGER_ADC_RETRY_MS (3000U)

static FuriPubSub* furi_hal_power_idle_pubsub = NULL;
static volatile bool furi_hal_power_idle = false;

static bool furi_hal_power_ls_policy_enabled = false;
static uint32_t furi_hal_power_ls_policy_delay_ms = 30000U;
static uint16_t furi_hal_power_ls_inhibit_count = 0;

#if CONFIG_PM_ENABLE
static FuriMutex* furi_hal_power_ls_mutex = NULL;
static bool furi_hal_power_ls_requested = false;

/* Held whenever insomnia > 0. Pins the CPU at max frequency so timing-critical
 * sections (bracketed with furi_hal_power_insomnia_enter/exit, e.g. every SPI
 * transaction) are not slowed by dynamic frequency scaling. NULL if the lock
 * could not be created, in which case insomnia becomes a no-op for DFS. */
static esp_pm_lock_handle_t furi_hal_power_freq_lock = NULL;

/* Master gate for automatic light sleep. Held by default so the SoC never
 * light-sleeps; released only when idle (screen off, on battery). Even when
 * released, IDF still only sleeps once every other lock is free (insomnia,
 * peripheral/RMT locks, the WiFi/BT drivers' own locks). */
static esp_pm_lock_handle_t furi_hal_power_no_ls_lock = NULL;
static bool furi_hal_power_ls_allowed = false;

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
/* Optional light-sleep instrumentation. Enable CONFIG_PM_LIGHT_SLEEP_CALLBACKS
 * to compile it in; the exit callback runs in IDLE-task context and just bumps
 * these counters, which furi_hal_power_get_light_sleep_stats() exposes. */
static volatile uint32_t furi_hal_power_ls_count = 0;
static volatile uint64_t furi_hal_power_ls_total_us = 0;
static volatile uint32_t furi_hal_power_ls_wake_timer = 0;
static volatile uint32_t furi_hal_power_ls_wake_gpio = 0;
static volatile uint32_t furi_hal_power_ls_wake_other = 0;

static esp_err_t furi_hal_power_ls_exit_cb(int64_t sleep_time_us, void* arg) {
    UNUSED(arg);
    furi_hal_power_ls_count++;
    if(sleep_time_us > 0) furi_hal_power_ls_total_us += (uint64_t)sleep_time_us;
    switch(esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_TIMER: furi_hal_power_ls_wake_timer++; break;
    case ESP_SLEEP_WAKEUP_GPIO:  furi_hal_power_ls_wake_gpio++;  break;
    default:                     furi_hal_power_ls_wake_other++; break;
    }
    return ESP_OK;
}
static esp_pm_sleep_cbs_register_config_t furi_hal_power_ls_cbs = {
    .exit_cb = furi_hal_power_ls_exit_cb,
};
#endif
#endif

#define FURI_HAL_POWER_USB_PRESENT_THRESHOLD_V  (4.6f)
#define FURI_HAL_POWER_LOW_BATTERY_THRESHOLD_V  (3.35f)
#define FURI_HAL_POWER_EMPTY_BATTERY_VOLTAGE_V  (3.27f)
#define FURI_HAL_POWER_FULL_BATTERY_VOLTAGE_V   (4.20f)
#ifndef BOARD_BATTERY_DIVIDER_TOP_OHM
#define BOARD_BATTERY_DIVIDER_TOP_OHM 510000UL
#endif
#ifndef BOARD_BATTERY_DIVIDER_BOTTOM_OHM
#define BOARD_BATTERY_DIVIDER_BOTTOM_OHM 255000UL
#endif
#ifndef BOARD_BATTERY_ADC_CORRECTION_NUMERATOR
#define BOARD_BATTERY_ADC_CORRECTION_NUMERATOR 1000UL
#endif
#ifndef BOARD_BATTERY_ADC_CORRECTION_DENOMINATOR
#define BOARD_BATTERY_ADC_CORRECTION_DENOMINATOR 1000UL
#endif
#define FURI_HAL_POWER_ADC_DIVIDER_RATIO \
    ((float)(BOARD_BATTERY_DIVIDER_TOP_OHM + BOARD_BATTERY_DIVIDER_BOTTOM_OHM) / \
     (float)BOARD_BATTERY_DIVIDER_BOTTOM_OHM)
#define FURI_HAL_POWER_SAMPLE_REFRESH_US        (1000000LL)
#define FURI_HAL_POWER_VOLTAGE_FILTER_TAU_US   (8000000LL)
#define FURI_HAL_POWER_VOLTAGE_MAX_STEP_V      (0.035f)
#define FURI_HAL_POWER_PCT_CONFIRM_US          (10000000LL)
#define FURI_HAL_POWER_ADC_FALLBACK_VREF_MV    1100
#define FURI_HAL_POWER_ADC_FALLBACK_FULL_SCALE_MV 3100
#define FURI_HAL_POWER_CHARGE_LIMIT_MIN_V       (3.840f)
#define FURI_HAL_POWER_CHARGE_LIMIT_MAX_V       (4.208f)
#define FURI_HAL_POWER_CHARGE_LIMIT_STEP_V      (0.016f)

typedef struct {
    bool initialized;
    bool init_attempted;
    bool adc_ready;
    bool adc_cali_ready;
    bool last_sample_ok;
    bool otg_enabled;
    bool has_battery_reading;
    uint8_t insomnia;
    uint8_t suppress_charge;
    uint8_t pct_candidate;
    int64_t last_sample_us;
    int64_t pct_candidate_since_us;
    float charge_voltage_limit;
    float last_supply_voltage;
    float last_battery_voltage;
    float last_adc_battery_voltage;
    float last_charger_battery_voltage;
    float filtered_battery_voltage;
    int last_adc_raw;
    int last_adc_pin_mv;
    uint8_t last_target_pct;
    uint8_t filtered_pct;
    bool filter_initialized;
    bool pct_valid;
    bool battery_voltage_from_adc;
    adc_unit_t adc_unit;
    adc_channel_t adc_channel;
    adc_oneshot_unit_handle_t adc_handle;
    adc_cali_handle_t adc_cali_handle;
} FuriHalPowerState;

typedef struct {
    float voltage;
    uint8_t pct;
} FuriHalPowerCurvePoint;

static FuriHalPowerState furi_hal_power = {
    .initialized = false,
    .init_attempted = false,
    .adc_ready = false,
    .adc_cali_ready = false,
    .last_sample_ok = false,
    .otg_enabled = false,
    .has_battery_reading = false,
    .insomnia = 0,
    .suppress_charge = 0,
    .pct_candidate = 100,
    .last_sample_us = 0,
    .pct_candidate_since_us = 0,
    .charge_voltage_limit = FURI_HAL_POWER_FULL_BATTERY_VOLTAGE_V,
    .last_supply_voltage = 0.0f,
    .last_battery_voltage = FURI_HAL_POWER_FULL_BATTERY_VOLTAGE_V,
    .last_adc_battery_voltage = 0.0f,
    .last_charger_battery_voltage = 0.0f,
    .filtered_battery_voltage = FURI_HAL_POWER_FULL_BATTERY_VOLTAGE_V,
    .last_adc_raw = 0,
    .last_adc_pin_mv = 0,
    .last_target_pct = 100,
    .filtered_pct = 100,
    .filter_initialized = false,
    .pct_valid = false,
    .battery_voltage_from_adc = false,
};

static bool furi_hal_power_shutdown_in_progress = false;

static const FuriHalPowerCurvePoint furi_hal_power_curve[] = {
    {3.27f, 0},
    {3.61f, 5},
    {3.69f, 10},
    {3.71f, 20},
    {3.73f, 30},
    {3.75f, 40},
    {3.77f, 50},
    {3.79f, 60},
    {3.82f, 70},
    {3.87f, 80},
    {3.94f, 90},
    {4.20f, 100},
};

#if BOARD_HAS_BATTERY_ADC
static bool furi_hal_power_adc_calibration_init(
    adc_unit_t unit,
    adc_channel_t channel,
    adc_atten_t atten,
    adc_cali_handle_t* out_handle) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = unit,
        .chan = channel,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if(adc_cali_create_scheme_curve_fitting(&cali_config, out_handle) == ESP_OK) {
        return true;
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = unit,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .default_vref = FURI_HAL_POWER_ADC_FALLBACK_VREF_MV,
    };
    if(adc_cali_create_scheme_line_fitting(&cali_config, out_handle) == ESP_OK) {
        return true;
    }
#endif
    return false;
}
#endif

/* Charger VREG steps are 16 mV starting at 3.840 V. Work in integer mV so that
 * e.g. 4.192f (4191.99..) is not truncated one step down; values between
 * steps are still truncated down, as the unit tests expect. */
static float furi_hal_power_quantize_charge_limit(float voltage) {
    float clamped = voltage;
    if(clamped < FURI_HAL_POWER_CHARGE_LIMIT_MIN_V) {
        clamped = FURI_HAL_POWER_CHARGE_LIMIT_MIN_V;
    } else if(clamped > FURI_HAL_POWER_CHARGE_LIMIT_MAX_V) {
        clamped = FURI_HAL_POWER_CHARGE_LIMIT_MAX_V;
    }

    const long min_mv = lroundf(FURI_HAL_POWER_CHARGE_LIMIT_MIN_V * 1000.0f);
    const long step_mv = lroundf(FURI_HAL_POWER_CHARGE_LIMIT_STEP_V * 1000.0f);
    const long mv = lroundf(clamped * 1000.0f);
    const long quantized_mv = min_mv + ((mv - min_mv) / step_mv) * step_mv;
    return (float)quantized_mv / 1000.0f;
}

static uint8_t furi_hal_power_voltage_to_pct(float voltage) {
    if(voltage <= furi_hal_power_curve[0].voltage) {
        return furi_hal_power_curve[0].pct;
    }

    for(size_t i = 1; i < COUNT_OF(furi_hal_power_curve); i++) {
        const FuriHalPowerCurvePoint* previous = &furi_hal_power_curve[i - 1];
        const FuriHalPowerCurvePoint* current = &furi_hal_power_curve[i];
        if(voltage <= current->voltage) {
            const float range = current->voltage - previous->voltage;
            if(range <= 0.0f) {
                return current->pct;
            }

            const float factor = (voltage - previous->voltage) / range;
            return (uint8_t)lroundf(previous->pct + factor * (current->pct - previous->pct));
        }
    }

    return 100;
}

static void furi_hal_power_ensure_initialized(void) {
    if(furi_hal_power.init_attempted) {
        return;
    }

    furi_hal_power.init_attempted = true;
    furi_hal_power.charge_voltage_limit =
        furi_hal_power_quantize_charge_limit(FURI_HAL_POWER_FULL_BATTERY_VOLTAGE_V);

#if !BOARD_HAS_BATTERY_ADC
    furi_hal_power.initialized = true;
    ESP_LOGI(
        TAG,
        "Battery ADC: not fitted on board %s, not initialized; using %s",
        BOARD_NAME,
        FURI_HAL_POWER_HAS_GAUGE ? "fuel gauge (charger VBAT fallback)" : "charger VBAT");
#else
    adc_unit_t adc_unit = ADC_UNIT_1;
    adc_channel_t adc_channel = ADC_CHANNEL_0;
    const esp_err_t map_result =
        adc_oneshot_io_to_channel(gpio_battery_sense.pin, &adc_unit, &adc_channel);
    if(map_result != ESP_OK) {
        ESP_LOGW(TAG, "Unable to map BAT_ADC GPIO%u: %s", gpio_battery_sense.pin, esp_err_to_name(map_result));
        return;
    }

    const adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = adc_unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    const esp_err_t unit_result =
        adc_oneshot_new_unit(&init_config, &furi_hal_power.adc_handle);
    if(unit_result != ESP_OK) {
        ESP_LOGW(TAG, "Unable to initialize ADC unit: %s", esp_err_to_name(unit_result));
        return;
    }

    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    const esp_err_t channel_result =
        adc_oneshot_config_channel(furi_hal_power.adc_handle, adc_channel, &channel_config);
    if(channel_result != ESP_OK) {
        ESP_LOGW(TAG, "Unable to configure BAT_ADC channel: %s", esp_err_to_name(channel_result));
        return;
    }

    furi_hal_power.adc_unit = adc_unit;
    furi_hal_power.adc_channel = adc_channel;
    furi_hal_power.adc_ready = true;
    furi_hal_power.adc_cali_ready = furi_hal_power_adc_calibration_init(
        adc_unit, adc_channel, channel_config.atten, &furi_hal_power.adc_cali_handle);
    furi_hal_power.initialized = true;
    ESP_LOGI(
        TAG,
        "Battery ADC ready: board=%s GPIO%u ADC unit=%d channel=%d divider=%lu/%lu ratio=%.3f calibration=%s source=%s",
        BOARD_NAME,
        (unsigned)gpio_battery_sense.pin,
        (int)adc_unit,
        (int)adc_channel,
        (unsigned long)BOARD_BATTERY_DIVIDER_TOP_OHM,
        (unsigned long)BOARD_BATTERY_DIVIDER_BOTTOM_OHM,
        FURI_HAL_POWER_ADC_DIVIDER_RATIO,
        furi_hal_power.adc_cali_ready ? "factory" : "fallback",
#if defined(BOARD_BATTERY_ADC_PRIMARY) && BOARD_BATTERY_ADC_PRIMARY
        "gpio-primary"
#else
        "auto"
#endif
    );
#endif /* BOARD_HAS_BATTERY_ADC */
}

static bool furi_hal_power_light_sleep_gate_open(void);

bool furi_hal_power_refresh_vbus_cache(void) {
    if(!furi_hal_bq25896_is_present()) return false;
    bool present = true;
    if(!furi_hal_bq25896_read_vbus_good(&present)) return false;
    FURI_CRITICAL_ENTER();
    furi_hal_power_vbus_present = present;
    furi_hal_power_vbus_valid = true;
    furi_hal_power_vbus_tick = furi_get_tick();
    FURI_CRITICAL_EXIT();
    return true;
}

void furi_hal_power_set_usb_deep_sleep_check(bool enabled, uint32_t interval_seconds) {
    if(interval_seconds < FURI_HAL_POWER_USB_DSLP_MIN_CHECK_S) {
        interval_seconds = FURI_HAL_POWER_USB_DSLP_MIN_CHECK_S;
    } else if(interval_seconds > FURI_HAL_POWER_USB_DSLP_MAX_CHECK_S) {
        interval_seconds = FURI_HAL_POWER_USB_DSLP_MAX_CHECK_S;
    }
    furi_hal_power_usb_dslp_check_enabled = enabled;
    furi_hal_power_usb_dslp_check_interval_s = interval_seconds;
}

/* Cached VBUS state. Refreshed at most once per FURI_HAL_POWER_VBUS_CACHE_MS
 * while awake and never while the light-sleep gate is open: in light sleep the
 * value read right before sleeping is used, so the charger is not polled. */
static bool furi_hal_power_get_vbus_state(bool* present) {
    if(!furi_hal_bq25896_is_present()) return false;
    const uint32_t age = furi_get_tick() - furi_hal_power_vbus_tick;
    /* furi_hal_power_idle covers the whole light-sleep state, including the
     * moments the gate is held closed by a short inhibitor. */
    if(!furi_hal_power_light_sleep_gate_open() && !furi_hal_power_idle &&
       (!furi_hal_power_vbus_valid || age >= furi_ms_to_ticks(FURI_HAL_POWER_VBUS_CACHE_MS))) {
        furi_hal_power_refresh_vbus_cache();
    }
    FURI_CRITICAL_ENTER();
    const bool valid = furi_hal_power_vbus_valid;
    *present = furi_hal_power_vbus_present;
    FURI_CRITICAL_EXIT();
    return valid;
}

static bool furi_hal_power_is_usb_present(void) {
    if(furi_hal_bq25896_is_present()) {
        bool present = false;
        return furi_hal_power_get_vbus_state(&present) && present;
    }
    return furi_hal_power.last_supply_voltage >= FURI_HAL_POWER_USB_PRESENT_THRESHOLD_V;
}

static void furi_hal_power_refresh_sample(void) {
#if !defined(BOARD_BATTERY_ADC_PRIMARY) || !BOARD_BATTERY_ADC_PRIMARY
    static uint32_t charger_vbat_invalid_log_tick = 0;
    static bool charger_vbat_invalid_logged = false;
#endif
    static bool adc_sample_invalid_logged = false;
    furi_hal_power_ensure_initialized();
#if BOARD_HAS_BATTERY_ADC
    if(!furi_hal_power.adc_ready) {
        furi_hal_power.last_sample_ok = false;
        return;
    }
#endif

    const int64_t now = esp_timer_get_time();
    if((now - furi_hal_power.last_sample_us) < FURI_HAL_POWER_SAMPLE_REFRESH_US &&
       furi_hal_power.last_sample_ok) {
        return;
    }
#if !BOARD_HAS_BATTERY_ADC
    /* The charger ADC is off during light sleep: its VBAT register is stale,
     * keep the previous sample instead of filtering an old value again. */
    if(!furi_hal_power_charger_adc_enabled) {
        /* It is off on purpose only while light sleep is granted. Awake with it still
         * off well after it was switched off means the re-enable on wake-up failed (an
         * I2C error): nothing would ever retry, so the battery readings would stay
         * frozen. The grace period keeps the few milliseconds between "ADC off" and
         * "light sleep granted" out of this; the path runs at most once per sample
         * refresh and never during light sleep (no charger polling there). */
        if(!furi_hal_power_idle && !furi_hal_power_light_sleep_gate_open() &&
           (furi_get_tick() - furi_hal_power_charger_adc_off_tick) >=
               furi_ms_to_ticks(FURI_HAL_POWER_CHARGER_ADC_RETRY_MS)) {
            furi_hal_power_set_charger_adc_enabled(true);
        }
        if(!furi_hal_power_charger_adc_enabled && furi_hal_power.last_sample_ok) {
            return;
        }
    }
#endif

    int raw_value = 0;
    int pin_mv = 0;
    float adc_battery_voltage = 0.0f;
#if BOARD_HAS_BATTERY_ADC
    int raw_sum = 0;
    int raw_min = INT_MAX;
    int raw_max = INT_MIN;
    bool adc_sample_ok = true;
    for(size_t sample = 0; sample < 16; sample++) {
        if(adc_oneshot_read(
               furi_hal_power.adc_handle, furi_hal_power.adc_channel, &raw_value) != ESP_OK) {
            adc_sample_ok = false;
            break;
        }
        raw_sum += raw_value;
        if(raw_value < raw_min) raw_min = raw_value;
        if(raw_value > raw_max) raw_max = raw_value;
    }

    if(adc_sample_ok) {
        raw_value = (raw_sum - raw_min - raw_max + 7) / 14;
        if(furi_hal_power.adc_cali_ready) {
            if(adc_cali_raw_to_voltage(furi_hal_power.adc_cali_handle, raw_value, &pin_mv) != ESP_OK) {
                adc_sample_ok = false;
            }
        } else {
            /* No eFuse calibration: use the ESP32-S3 12 dB nominal range, not
             * the 3.3 V supply rail. The latter overestimates VBAT materially. */
            pin_mv = (raw_value * FURI_HAL_POWER_ADC_FALLBACK_FULL_SCALE_MV) / 4095;
        }

        if(adc_sample_ok) {
            if(pin_mv < 0) pin_mv = 0;
            if(pin_mv > FURI_HAL_POWER_ADC_FALLBACK_FULL_SCALE_MV) {
                pin_mv = FURI_HAL_POWER_ADC_FALLBACK_FULL_SCALE_MV;
            }

            pin_mv = (int)(((int64_t)pin_mv * BOARD_BATTERY_ADC_CORRECTION_NUMERATOR +
                            BOARD_BATTERY_ADC_CORRECTION_DENOMINATOR / 2) /
                           BOARD_BATTERY_ADC_CORRECTION_DENOMINATOR);
            adc_battery_voltage =
                ((float)pin_mv / 1000.0f) * FURI_HAL_POWER_ADC_DIVIDER_RATIO;
            adc_sample_ok = adc_battery_voltage >= 2.5f && adc_battery_voltage <= 4.5f;
        }
    }
#else
    const bool adc_sample_ok = false;
#endif

    furi_hal_power.last_adc_raw = raw_value;
    furi_hal_power.last_adc_pin_mv = pin_mv;
    furi_hal_power.last_adc_battery_voltage = adc_battery_voltage;

    /* The board divider is the primary battery measurement. Boards that
     * declare it authoritative must not substitute the charger's VBAT ADC. */
    float charger_battery_voltage = 0.0f;
#if !defined(BOARD_BATTERY_ADC_PRIMARY) || !BOARD_BATTERY_ADC_PRIMARY
    if(furi_hal_bq25896_is_present()) {
        const uint16_t charger_vbat_mv = furi_hal_bq25896_get_vbat_voltage_mv();
        if(charger_vbat_mv >= 2500U && charger_vbat_mv <= 4500U) {
            charger_battery_voltage = (float)charger_vbat_mv / 1000.0f;
        } else {
            /* Without a cell this alternates between valid and invalid every
             * conversion; rate limit instead of logging each transition. */
            const uint32_t now_tick = furi_get_tick();
            if(!charger_vbat_invalid_logged ||
               (now_tick - charger_vbat_invalid_log_tick) >=
                   furi_ms_to_ticks(FURI_HAL_POWER_LOG_INTERVAL_MS)) {
                charger_vbat_invalid_logged = true;
                charger_vbat_invalid_log_tick = now_tick;
                ESP_LOGW(
                    TAG,
                    "BQ25896 VBAT reading invalid: %umV (no battery?); %s",
                    charger_vbat_mv,
                    BOARD_HAS_BATTERY_ADC ? "using divider" : "no battery voltage source");
            }
        }
    }
#endif
    furi_hal_power.last_charger_battery_voltage = charger_battery_voltage;
    furi_hal_power.battery_voltage_from_adc = adc_sample_ok;
    if(!adc_sample_ok && charger_battery_voltage <= 0.0f) {
        furi_hal_power.last_sample_ok = false;
        if(BOARD_HAS_BATTERY_ADC && !adc_sample_invalid_logged) {
            ESP_LOGW(
                TAG,
                "Battery ADC sample invalid (raw=%d pin=%dmV); no valid battery source",
                raw_value,
                pin_mv);
            adc_sample_invalid_logged = true;
        }
        return;
    }
    if(adc_sample_ok) {
        adc_sample_invalid_logged = false;
    } else if(BOARD_HAS_BATTERY_ADC && !adc_sample_invalid_logged) {
        ESP_LOGW(TAG, "Battery ADC sample invalid; using charger VBAT fallback");
        adc_sample_invalid_logged = true;
    }

    const float supply_voltage =
        adc_sample_ok ? adc_battery_voltage : charger_battery_voltage;
    furi_hal_power.last_supply_voltage = supply_voltage;
    furi_hal_power.has_battery_reading = true;
    float battery_voltage = supply_voltage;
    if(battery_voltage < FURI_HAL_POWER_EMPTY_BATTERY_VOLTAGE_V) {
        battery_voltage = FURI_HAL_POWER_EMPTY_BATTERY_VOLTAGE_V;
    } else if(battery_voltage > furi_hal_power.charge_voltage_limit) {
        battery_voltage = furi_hal_power.charge_voltage_limit;
    }

    const int64_t elapsed_us =
        furi_hal_power.last_sample_us > 0 ? now - furi_hal_power.last_sample_us :
                                             FURI_HAL_POWER_SAMPLE_REFRESH_US;
    if(!furi_hal_power.filter_initialized) {
        furi_hal_power.filtered_battery_voltage = battery_voltage;
        furi_hal_power.filter_initialized = true;
    } else {
        const float alpha = (float)elapsed_us /
                            (float)(FURI_HAL_POWER_VOLTAGE_FILTER_TAU_US + elapsed_us);
        float filtered = furi_hal_power.filtered_battery_voltage +
                         alpha * (battery_voltage - furi_hal_power.filtered_battery_voltage);
        const float delta = filtered - furi_hal_power.filtered_battery_voltage;
        if(delta > FURI_HAL_POWER_VOLTAGE_MAX_STEP_V) {
            filtered = furi_hal_power.filtered_battery_voltage + FURI_HAL_POWER_VOLTAGE_MAX_STEP_V;
        } else if(delta < -FURI_HAL_POWER_VOLTAGE_MAX_STEP_V) {
            filtered = furi_hal_power.filtered_battery_voltage - FURI_HAL_POWER_VOLTAGE_MAX_STEP_V;
        }
        furi_hal_power.filtered_battery_voltage = filtered;
    }
    furi_hal_power.last_battery_voltage = furi_hal_power.filtered_battery_voltage;
    const uint8_t target_pct =
        furi_hal_power_voltage_to_pct(furi_hal_power.filtered_battery_voltage);
    furi_hal_power.last_target_pct = target_pct;
    if(!furi_hal_power.pct_valid) {
        furi_hal_power.filtered_pct = target_pct;
        furi_hal_power.pct_candidate = target_pct;
        furi_hal_power.pct_candidate_since_us = now;
        furi_hal_power.pct_valid = true;
    } else if(target_pct == furi_hal_power.filtered_pct) {
        furi_hal_power.pct_candidate = target_pct;
        furi_hal_power.pct_candidate_since_us = now;
    } else {
        if(target_pct != furi_hal_power.pct_candidate) {
            furi_hal_power.pct_candidate = target_pct;
            furi_hal_power.pct_candidate_since_us = now;
        } else if((now - furi_hal_power.pct_candidate_since_us) >=
                  FURI_HAL_POWER_PCT_CONFIRM_US) {
            furi_hal_power.filtered_pct = target_pct;
        }
    }

    ESP_LOGD(
        TAG,
        "Battery sample: raw=%d pin=%dmV adc=%.3fV charger=%.3fV "
        "source=%s filtered=%.3fV pct=%u target=%u usb=%d",
        raw_value,
        pin_mv,
        (double)adc_battery_voltage,
        (double)charger_battery_voltage,
        adc_sample_ok ? "adc" : "charger",
        (double)furi_hal_power.last_battery_voltage,
        furi_hal_power.filtered_pct,
        target_pct,
        furi_hal_power_is_usb_present());

    furi_hal_power.last_sample_us = now;
    furi_hal_power.last_sample_ok = true;
}

/* Battery voltage used for protection and the fuel-gauge voltage field. On
 * gauge boards (paradoxt) this is BQ27220 Voltage(); the ADC/charger estimate
 * is only a fallback while the gauge snapshot is stale. */
static float furi_hal_power_get_estimated_battery_voltage(void) {
#if FURI_HAL_POWER_HAS_GAUGE && !FURI_HAL_POWER_ADC_PRIMARY
    if(furi_hal_bq27220_is_present()) {
        FuriHalBq27220Snapshot gauge;
        const bool valid = furi_hal_bq27220_get_snapshot(&gauge);
        /* No cell fitted (last answer said so): there is no battery voltage,
         * and the charger VBAT is meaningless too. 0 = "no reading", so the
         * protection holds instead of acting on noise. */
        if(valid && !gauge.battery_present) return 0.0f;
        if(valid && gauge.fresh) return (float)gauge.voltage_mv / 1000.0f;
        /* Gauge stopped answering with a cell fitted: charger VBAT fallback. */
    }
#endif
    furi_hal_power_refresh_sample();

    if(!furi_hal_power.last_sample_ok) {
        return 0.0f;
    }

    return furi_hal_power.last_battery_voltage;
}

static bool furi_hal_power_light_sleep_gate_open(void) {
#if CONFIG_PM_ENABLE
    return furi_hal_power_ls_allowed;
#else
    return false;
#endif
}

bool furi_hal_power_is_light_sleep_active(void) {
    return furi_hal_power_light_sleep_gate_open();
}

bool furi_hal_power_is_light_sleep_requested(void) {
#if CONFIG_PM_ENABLE
    return furi_hal_power_ls_requested;
#else
    return false;
#endif
}

void furi_hal_power_set_charger_adc_enabled(bool enabled) {
    if(furi_hal_bq25896_is_present() && !furi_hal_bq25896_set_adc_continuous(enabled)) {
        return;
    }
    if(!enabled) furi_hal_power_charger_adc_off_tick = furi_get_tick();
    furi_hal_power_charger_adc_enabled = enabled;
}

FuriPubSub* furi_hal_power_get_idle_pubsub(void) {
    return furi_hal_power_idle_pubsub;
}

void furi_hal_power_publish_idle_state(bool idle) {
    if(furi_hal_power_idle == idle) return;
    furi_hal_power_idle = idle;
    if(furi_hal_power_idle_pubsub) {
        const FuriHalPowerIdleEvent event = {.idle = idle};
        furi_pubsub_publish(furi_hal_power_idle_pubsub, (void*)&event);
    }
}

bool furi_hal_power_is_idle(void) {
    return furi_hal_power_idle;
}

static void furi_hal_power_usb_deep_sleep_check(void);

void furi_hal_power_init(void) {
    /* Idempotent: no-op bila sudah dijalankan oleh constructor boot awal. */
    furi_hal_power_release_gpio_holds();

    ESP_LOGW(
        TAG,
        "Boot diagnostics: reset_reason=%d wakeup_cause=%d",
        (int)esp_reset_reason(),
        (int)esp_sleep_get_wakeup_cause());

    furi_hal_power_ensure_initialized();

    if(!furi_hal_power_idle_pubsub) {
        furi_hal_power_idle_pubsub = furi_pubsub_alloc();
    }

#if CONFIG_PM_ENABLE
    if(!furi_hal_power_ls_mutex) {
        furi_hal_power_ls_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
        if(!furi_hal_power_ls_mutex) {
            ESP_LOGE(TAG, "Unable to create light-sleep gate mutex; automatic light sleep disabled");
        }
    }

    /* Dynamic frequency scaling: let the CPU idle at 80 MHz and ramp to 160 MHz
     * only while a peripheral driver (SPI / RMT / I2C ...) or a timing-critical
     * section (insomnia) needs it. This roughly halves idle CPU draw, which is
     * the dominant battery sink on this port. Nothing ever lowered the fixed
     * 160 MHz clock before.
     *
     * DFS 80-160 MHz plus automatic light sleep, the latter gated by the
     * NO_LIGHT_SLEEP lock below. min_freq is 80 (not 40) so APB stays
     * PLL-sourced and the LEDC backlight PWM and I2C timing don't shift. */
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };
    esp_err_t pm_err = esp_pm_configure(&pm_config);
    if(pm_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure failed: %s", esp_err_to_name(pm_err));
    } else {
        esp_err_t lock_err = esp_pm_lock_create(
            ESP_PM_CPU_FREQ_MAX, 0, "insomnia", &furi_hal_power_freq_lock);
        if(lock_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_pm_lock_create failed: %s", esp_err_to_name(lock_err));
            furi_hal_power_freq_lock = NULL;
        }

        /* Create the light-sleep gate and hold it, so nothing sleeps until the
         * idle state (screen off, on battery) explicitly permits it. */
        lock_err = esp_pm_lock_create(
            ESP_PM_NO_LIGHT_SLEEP, 0, "screen_on", &furi_hal_power_no_ls_lock);
        if(lock_err != ESP_OK) {
            furi_hal_power_no_ls_lock = NULL;
            ESP_LOGE(TAG, "NO_LIGHT_SLEEP lock unavailable; automatic light sleep disabled");
        } else {
            esp_pm_lock_acquire(furi_hal_power_no_ls_lock);
            pm_config.light_sleep_enable = true;
            pm_err = esp_pm_configure(&pm_config);
            if(pm_err != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "Could not enable gated automatic light sleep: %s",
                    esp_err_to_name(pm_err));
            } else {
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
                esp_pm_light_sleep_register_cbs(&furi_hal_power_ls_cbs);
#endif
                ESP_LOGI(TAG, "DFS 80-160 MHz + gated light sleep enabled");
            }
        }
    }
#endif

    /* Initialize shared I2C bus for power ICs (BQ27220 + BQ25896) */
#if defined(BOARD_PIN_QWIIC_SDA) && defined(BOARD_PIN_QWIIC_SCL)
    {
        i2c_config_t conf = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = BOARD_PIN_QWIIC_SDA,
            .scl_io_num = BOARD_PIN_QWIIC_SCL,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = 100000,
        };
        i2c_param_config(I2C_NUM_0, &conf);
        esp_err_t i2c_err = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
        if(i2c_err != ESP_OK && i2c_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Power I2C bus init failed: %s", esp_err_to_name(i2c_err));
        }
    }
#endif

    /* Allow power ICs to stabilize after PWR_EN and I2C bus init */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Smoochie has no BQ27220; its battery percentage/voltage come from the
     * board battery ADC divider. Other boards retain the legacy fuel-gauge path. */
#if !defined(BOARD_HAS_BQ27220) || BOARD_HAS_BQ27220
    furi_hal_bq27220_init();
#endif
    furi_hal_bq25896_init();
    furi_hal_power_usb_deep_sleep_check();
    furi_hal_power_refresh_vbus_cache();
    furi_hal_power_refresh_sample();
#if !defined(BOARD_HAS_BQ27220) || BOARD_HAS_BQ27220
    if(furi_hal_bq27220_is_present()) {
        ESP_LOGI(TAG, "Fuel gauge: BQ27220 (%umV, %u%%)",
            furi_hal_bq27220_get_voltage_mv(), furi_hal_bq27220_get_charge_pct());
    }
#else
    ESP_LOGI(TAG, "Fuel gauge: external BQ27220 disabled; using battery ADC");
#endif
    if(furi_hal_bq25896_is_present()) {
        ESP_LOGI(TAG, "Charger: BQ25896 (VBUS=%umV, VREG=%umV)",
            furi_hal_bq25896_get_vbus_voltage_mv(), furi_hal_bq25896_get_vreg_voltage_mv());
    }
}

bool furi_hal_power_gauge_is_ok(void) {
    /* With a gauge: only when it reports a fitted cell recently. Without a
     * battery the status bar then shows "no data" instead of stale values. */
    if(furi_hal_bq27220_is_present()) return furi_hal_bq27220_is_data_valid();
    /* Deliberately false without a gauge or a battery ADC, even though get_pct() can fall
     * back to the charger VBAT: with no cell fitted that VBAT alternates between valid
     * and invalid every conversion, which would make the status bar flicker a bogus
     * percentage instead of showing a steady "no data". */
    if(!furi_hal_power.adc_handle) return false;
    furi_hal_power_refresh_sample();
    return furi_hal_power.last_sample_ok;
}

typedef struct {
    bool started;
    bool low;
    bool critical;
    uint8_t low_samples;
    uint8_t critical_samples;
    uint32_t last_tick;
} FuriHalPowerProtection;

static FuriHalPowerProtection furi_hal_power_protection = {0};

static void furi_hal_power_protection_step(
    bool now,
    bool clear,
    uint8_t* samples,
    bool* latched,
    const char* name,
    float voltage) {
    if(now) {
        if(*samples < FURI_HAL_POWER_PROTECTION_SAMPLES) (*samples)++;
        if(*samples >= FURI_HAL_POWER_PROTECTION_SAMPLES && !*latched) {
            *latched = true;
            ESP_LOGW(TAG, "Battery %s level reached (%.3fV)", name, (double)voltage);
        }
    } else {
        *samples = 0;
        if(clear && *latched) {
            *latched = false;
            ESP_LOGI(TAG, "Battery %s level cleared (%.3fV)", name, (double)voltage);
        }
    }
}

/* Evaluated at most once per FURI_HAL_POWER_PROTECTION_INTERVAL_MS, so both
 * public queries share the same samples. Without a valid reading the previous
 * decision is held rather than guessed. */
static void furi_hal_power_update_protection(void) {
    FuriHalPowerProtection* p = &furi_hal_power_protection;
    const uint32_t now = furi_get_tick();
    if(p->started && (now - p->last_tick) < furi_ms_to_ticks(FURI_HAL_POWER_PROTECTION_INTERVAL_MS)) {
        return;
    }
    p->started = true;
    p->last_tick = now;

    if(furi_hal_power_shutdown_in_progress || furi_hal_power_is_usb_present()) {
        p->low = p->critical = false;
        p->low_samples = p->critical_samples = 0;
        return;
    }

    const float voltage = furi_hal_power_get_estimated_battery_voltage();
    bool sysdown = false;
#if FURI_HAL_POWER_HAS_GAUGE
    sysdown = furi_hal_bq27220_is_present() && furi_hal_bq27220_is_sysdown();
#endif
    const bool valid = voltage > 0.0f;
    if(!valid && !sysdown) return;

    furi_hal_power_protection_step(
        sysdown || (valid && voltage <= FURI_HAL_POWER_LOW_BATTERY_THRESHOLD_V),
        !sysdown && valid && voltage >= FURI_HAL_POWER_LOW_BATTERY_CLEAR_V,
        &p->low_samples,
        &p->low,
        "low",
        voltage);
    furi_hal_power_protection_step(
        sysdown || (valid && voltage <= FURI_HAL_POWER_CRITICAL_BATTERY_V),
        !sysdown && valid && voltage >= FURI_HAL_POWER_CRITICAL_BATTERY_CLEAR_V,
        &p->critical_samples,
        &p->critical,
        sysdown ? "critical (gauge SYSDWN)" : "critical",
        voltage);
}

bool furi_hal_power_is_shutdown_requested(void) {
    furi_hal_power_update_protection();
    return furi_hal_power_protection.low;
}

bool furi_hal_power_is_critical_shutdown_requested(void) {
    furi_hal_power_update_protection();
    return furi_hal_power_protection.critical;
}

uint16_t furi_hal_power_insomnia_level(void) {
    return furi_hal_power.insomnia;
}

void furi_hal_power_insomnia_enter(void) {
    /* Acquire the paired light-sleep reference before publishing an active
     * insomnia session; otherwise a concurrent exit can consume another
     * client's reference before this session has registered its own. */
    furi_hal_power_light_sleep_inhibit_enter();
    FURI_CRITICAL_ENTER();
    furi_check(furi_hal_power.insomnia < UINT8_MAX);
    const bool first = (furi_hal_power.insomnia == 0);
    furi_hal_power.insomnia++;
    FURI_CRITICAL_EXIT();
    /* The frequency switch may busy-wait for PLL relock, so take its lock
     * outside the critical section. Only the 0->1 transition needs a reference. */
#if CONFIG_PM_ENABLE
    if(first && furi_hal_power_freq_lock) {
        esp_pm_lock_acquire(furi_hal_power_freq_lock);
    }
#else
    (void)first;
#endif
}

void furi_hal_power_insomnia_exit(void) {
    FURI_CRITICAL_ENTER();
    furi_check(furi_hal_power.insomnia > 0);
    furi_hal_power.insomnia--;
    const bool last = (furi_hal_power.insomnia == 0);
    FURI_CRITICAL_EXIT();
    furi_hal_power_light_sleep_inhibit_exit();
#if CONFIG_PM_ENABLE
    if(last && furi_hal_power_freq_lock) {
        esp_pm_lock_release(furi_hal_power_freq_lock);
    }
#else
    (void)last;
#endif
}

bool furi_hal_power_sleep_available(void) {
    return furi_hal_power.insomnia == 0;
}

bool furi_hal_power_is_running_on_battery(void) {
    /* Only an explicit VBUS sense is trusted. Without a charger IC we can't
     * tell USB from battery, so assume wired and never light-sleep. */
    bool vbus_present = true;
    return furi_hal_power_get_vbus_state(&vbus_present) && !vbus_present;
}

bool furi_hal_power_is_usb_connected(void) {
    bool vbus_present = false;
    return furi_hal_power_get_vbus_state(&vbus_present) && vbus_present;
}

static bool furi_hal_power_pin_number_valid(gpio_num_t pin) {
    return (int)pin >= 0 && (int)pin < (int)GPIO_NUM_MAX && GPIO_IS_VALID_GPIO(pin);
}

/* Pads that a driver wants kept HIGH (weak pull-up) during light sleep, overriding the
 * table below. Few and short-lived: the chip-selects of an attached header module. */
#define FURI_HAL_POWER_LS_HOLD_MAX 4
static int16_t furi_hal_power_ls_hold_pins[FURI_HAL_POWER_LS_HOLD_MAX] = {-1, -1, -1, -1};

bool furi_hal_power_light_sleep_hold_pin_high(uint16_t pin, bool hold) {
    if(!furi_hal_power_pin_number_valid((gpio_num_t)pin)) return false;
    bool ok = true;
    FURI_CRITICAL_ENTER();
    int free_slot = -1;
    int found = -1;
    for(int i = 0; i < FURI_HAL_POWER_LS_HOLD_MAX; i++) {
        if(furi_hal_power_ls_hold_pins[i] == (int16_t)pin) found = i;
        if(furi_hal_power_ls_hold_pins[i] < 0 && free_slot < 0) free_slot = i;
    }
    if(hold) {
        if(found < 0) {
            if(free_slot >= 0) {
                furi_hal_power_ls_hold_pins[free_slot] = (int16_t)pin;
            } else {
                ok = false;
            }
        }
    } else if(found >= 0) {
        furi_hal_power_ls_hold_pins[found] = -1;
    }
    FURI_CRITICAL_EXIT();
    return ok;
}

#if CONFIG_PM_ENABLE
/* ------------------------------------------------------------------------
 * Light-sleep pad states.
 *
 * CONFIG_PM_SLP_DISABLE_GPIO (forced on by ESP_SLEEP_GPIO_RESET_WORKAROUND on
 * the S3) isolates every pad during automatic light sleep: input, output and
 * pulls off. The chip side then draws nothing, but the external inputs wired to
 * those pads float: LCD RST (panel reset), CC1101 CSn (wakes the radio out of
 * SLEEP), the backlight / IR / buzzer transistor gates, the WS2812 data line.
 * Each connected pad gets a defined sleep state here. Input buffers stay off
 * (that is what the ESD workaround is about); a weak pull holds the level.
 * Button pads are wake sources and are handled by the input driver
 * (gpio_wakeup_enable keeps their normal pull-up configuration).
 * ------------------------------------------------------------------------ */
typedef enum {
    FuriHalPowerLsPinPullUp, /* input/output off, pull-up: idle-high inputs, CS, RST */
    FuriHalPowerLsPinPullDown, /* input/output off, pull-down: idle-low lines, gates */
    FuriHalPowerLsPinNoPull, /* input/output off, externally biased */
    FuriHalPowerLsPinKeepOutput, /* keep driving the current output level (power enables) */
} FuriHalPowerLsPinMode;

typedef struct {
    gpio_num_t pin;
    FuriHalPowerLsPinMode mode;
} FuriHalPowerLsPinEntry;

/* Board pads beyond the common LCD/LED/I2C set, comma separated with a trailing
 * comma, e.g. {(gpio_num_t)BOARD_PIN_SD_CS, FuriHalPowerLsPinPullUp}, */
#ifndef FURI_HAL_POWER_LIGHT_SLEEP_EXTRA_PINS
#define FURI_HAL_POWER_LIGHT_SLEEP_EXTRA_PINS
#endif

static const FuriHalPowerLsPinEntry furi_hal_power_ls_pins[] = {
#ifdef BOARD_PIN_PWR_EN
    {(gpio_num_t)BOARD_PIN_PWR_EN, FuriHalPowerLsPinKeepOutput},
#endif
#ifdef BOARD_PIN_WS2812_DATA
    /* Held low: no stray edge can latch a colour into the (already black) LEDs. */
    {(gpio_num_t)BOARD_PIN_WS2812_DATA, FuriHalPowerLsPinPullDown},
#endif
#ifdef BOARD_PIN_LCD_BL
    {(gpio_num_t)BOARD_PIN_LCD_BL,
     BOARD_LCD_BL_ACTIVE_LOW ? FuriHalPowerLsPinPullUp : FuriHalPowerLsPinPullDown},
#endif
#ifdef BOARD_PIN_LCD_RST
    /* Must stay high: a reset would drop the panel configuration (wake only sends SLPOUT). */
    {(gpio_num_t)BOARD_PIN_LCD_RST, FuriHalPowerLsPinPullUp},
#endif
#ifdef BOARD_PIN_LCD_CS
    {(gpio_num_t)BOARD_PIN_LCD_CS, FuriHalPowerLsPinPullUp},
#endif
#ifdef BOARD_PIN_LCD_DC
    {(gpio_num_t)BOARD_PIN_LCD_DC, FuriHalPowerLsPinPullDown},
#endif
#ifdef BOARD_PIN_LCD_MOSI
    {(gpio_num_t)BOARD_PIN_LCD_MOSI, FuriHalPowerLsPinPullDown},
#endif
#ifdef BOARD_PIN_LCD_SCLK
    {(gpio_num_t)BOARD_PIN_LCD_SCLK, FuriHalPowerLsPinPullDown},
#endif
#if defined(BOARD_PIN_QWIIC_SDA) && defined(BOARD_PIN_QWIIC_SCL)
    {(gpio_num_t)BOARD_PIN_QWIIC_SDA, FuriHalPowerLsPinPullUp},
    {(gpio_num_t)BOARD_PIN_QWIIC_SCL, FuriHalPowerLsPinPullUp},
#endif
    FURI_HAL_POWER_LIGHT_SLEEP_EXTRA_PINS
    {GPIO_NUM_NC, FuriHalPowerLsPinNoPull}, /* terminator, skipped */
};

/* Sleep-mode pad registers are separate from the run-mode ones, so this does
 * not disturb the awake configuration. Re-applied on every gate opening in
 * case a driver re-enabled or disabled the sleep selection meanwhile. */
static void furi_hal_power_apply_light_sleep_pins(void) {
    static bool logged = false;
    unsigned applied = 0;
    for(size_t i = 0; i < COUNT_OF(furi_hal_power_ls_pins); i++) {
        const gpio_num_t pin = furi_hal_power_ls_pins[i].pin;
        if(!furi_hal_power_pin_number_valid(pin)) continue;
        if((int)pin == (int)gpio_battery_sense.pin) continue;
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
        gpio_sleep_sel_en(pin);
        applied++;
    }
    /* Pads an attached module needs deselected (see furi_hal_power_light_sleep_hold_pin_high);
     * applied last so they win over the table default. A pad that was released since the
     * previous sleep is back at its table state because the table is re-applied above. */
    int16_t held[FURI_HAL_POWER_LS_HOLD_MAX];
    FURI_CRITICAL_ENTER();
    for(int i = 0; i < FURI_HAL_POWER_LS_HOLD_MAX; i++) held[i] = furi_hal_power_ls_hold_pins[i];
    FURI_CRITICAL_EXIT();
    for(int i = 0; i < FURI_HAL_POWER_LS_HOLD_MAX; i++) {
        if(held[i] < 0) continue;
        const gpio_num_t pin = (gpio_num_t)held[i];
        gpio_sleep_set_direction(pin, GPIO_MODE_DISABLE);
        gpio_sleep_set_pull_mode(pin, GPIO_PULLUP_ONLY);
        gpio_sleep_sel_en(pin);
        applied++;
    }
    if(!logged) {
        logged = true;
        ESP_LOGI(TAG, "Light-sleep pad states configured for %u pins", applied);
    }
}

/* Gate condition. Cheap checks first, and no I2C at all: VBUS comes from the
 * cache the power service refreshes right before it grants light sleep. */
static bool device_can_enter_light_sleep(void) {
    uint16_t active_sessions;
    FURI_CRITICAL_ENTER();
    active_sessions = furi_hal_power_ls_inhibit_count;
    const bool on_battery = furi_hal_power_vbus_valid && !furi_hal_power_vbus_present;
    FURI_CRITICAL_EXIT();

    /* Boost (OTG) on: a module on the header is powered from it and the OTG
     * fault supervision runs in the awake power tick only. Cached flag, no I2C. */
    return active_sessions == 0 && furi_hal_power_insomnia_level() == 0 &&
           !furi_hal_power.otg_enabled &&
           furi_hal_display_is_asleep() && !furi_hal_speaker_is_playing() &&
           !furi_hal_bt_extra_beacon_is_active() && furi_hal_bq25896_is_present() && on_battery;
}

static void furi_hal_power_update_light_sleep_gate_locked(void) {
    if(!furi_hal_power_no_ls_lock) return;
    const bool can_sleep =
        furi_hal_power_ls_requested && device_can_enter_light_sleep();
    if(can_sleep == furi_hal_power_ls_allowed) return;
    if(can_sleep) furi_hal_power_apply_light_sleep_pins();
    const esp_err_t err = can_sleep ?
        esp_pm_lock_release(furi_hal_power_no_ls_lock) :
        esp_pm_lock_acquire(furi_hal_power_no_ls_lock);
    if(err == ESP_OK) {
        furi_hal_power_ls_allowed = can_sleep;
    } else {
        ESP_LOGE(TAG, "Failed to update light-sleep gate: %s", esp_err_to_name(err));
    }
}
#endif

static FuriHalPowerLightSleepRevokeCallback furi_hal_power_ls_revoke_callback = NULL;
static void* furi_hal_power_ls_revoke_context = NULL;

void furi_hal_power_set_light_sleep_revoke_callback(
    FuriHalPowerLightSleepRevokeCallback callback,
    void* context) {
    FURI_CRITICAL_ENTER();
    furi_hal_power_ls_revoke_callback = callback;
    furi_hal_power_ls_revoke_context = context;
    FURI_CRITICAL_EXIT();
}

void furi_hal_power_allow_light_sleep(bool allow) {
#if CONFIG_PM_ENABLE
    if(!furi_hal_power_ls_mutex ||
       furi_mutex_acquire(furi_hal_power_ls_mutex, FuriWaitForever) != FuriStatusOk) {
        return;
    }
    const bool revoked = furi_hal_power_ls_requested && !allow;
    furi_hal_power_ls_requested = allow;
    furi_hal_power_update_light_sleep_gate_locked();
    furi_mutex_release(furi_hal_power_ls_mutex);

    if(revoked) {
        FURI_CRITICAL_ENTER();
        FuriHalPowerLightSleepRevokeCallback callback = furi_hal_power_ls_revoke_callback;
        void* context = furi_hal_power_ls_revoke_context;
        FURI_CRITICAL_EXIT();
        if(callback) callback(context);
    }
#else
    (void)allow;
#endif
}

void furi_hal_power_set_light_sleep_policy(bool enabled, uint32_t delay_ms) {
    FURI_CRITICAL_ENTER();
    furi_hal_power_ls_policy_enabled = enabled;
    furi_hal_power_ls_policy_delay_ms = delay_ms;
    FURI_CRITICAL_EXIT();
    if(!enabled) {
        furi_hal_power_allow_light_sleep(false);
    }
}

bool furi_hal_power_get_light_sleep_policy(uint32_t* delay_ms) {
    FURI_CRITICAL_ENTER();
    const bool enabled = furi_hal_power_ls_policy_enabled;
    if(delay_ms) *delay_ms = furi_hal_power_ls_policy_delay_ms;
    FURI_CRITICAL_EXIT();
    return enabled;
}

/* Inhibits may be taken before furi_hal_power_init() created the gate mutex
 * (e.g. an SPI transaction during early HAL init). The counter is still kept;
 * the gate itself is held closed from creation until explicitly granted. */
void furi_hal_power_light_sleep_inhibit_enter(void) {
#if CONFIG_PM_ENABLE
    const bool locked = furi_hal_power_ls_mutex &&
                        furi_mutex_acquire(furi_hal_power_ls_mutex, FuriWaitForever) ==
                            FuriStatusOk;
#endif
    FURI_CRITICAL_ENTER();
    furi_check(furi_hal_power_ls_inhibit_count < UINT16_MAX);
    furi_hal_power_ls_inhibit_count++;
    FURI_CRITICAL_EXIT();
#if CONFIG_PM_ENABLE
    if(locked) {
        furi_hal_power_update_light_sleep_gate_locked();
        furi_mutex_release(furi_hal_power_ls_mutex);
    }
#endif
}

void furi_hal_power_light_sleep_inhibit_exit(void) {
#if CONFIG_PM_ENABLE
    const bool locked = furi_hal_power_ls_mutex &&
                        furi_mutex_acquire(furi_hal_power_ls_mutex, FuriWaitForever) ==
                            FuriStatusOk;
#endif
    FURI_CRITICAL_ENTER();
    furi_check(furi_hal_power_ls_inhibit_count > 0);
    furi_hal_power_ls_inhibit_count--;
    FURI_CRITICAL_EXIT();
#if CONFIG_PM_ENABLE
    if(locked) {
        furi_hal_power_update_light_sleep_gate_locked();
        furi_mutex_release(furi_hal_power_ls_mutex);
    }
#endif
}

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
void furi_hal_power_get_light_sleep_stats(FuriHalPowerLightSleepStats* out) {
    if(!out) return;
    out->sleep_count = furi_hal_power_ls_count;
    out->total_sleep_us = furi_hal_power_ls_total_us;
    out->wake_timer = furi_hal_power_ls_wake_timer;
    out->wake_gpio = furi_hal_power_ls_wake_gpio;
    out->wake_other = furi_hal_power_ls_wake_other;
    out->allowed = furi_hal_power_ls_allowed;
}
#endif

void furi_hal_power_sleep(void) {
    vTaskDelay(pdMS_TO_TICKS(1));
}

uint8_t furi_hal_power_get_pct(void) {
#if (!defined(BOARD_BATTERY_ADC_PRIMARY) || !BOARD_BATTERY_ADC_PRIMARY) && \
    (!defined(BOARD_HAS_BQ27220) || BOARD_HAS_BQ27220)
    /* BQ27220 fuel gauge takes priority only on boards that actually have it. */
    if(furi_hal_bq27220_is_present()) {
        return furi_hal_bq27220_get_charge_pct();
    }
#endif
    furi_hal_power_ensure_initialized();
#if BOARD_HAS_BATTERY_ADC
    if(!furi_hal_power.adc_ready) {
        return 0; // fix: gagal init ADC -> tampilkan 0%, bukan 100% palsu
    }
#endif
    /* ADC divider, or charger VBAT on boards with neither ADC nor gauge. */
    furi_hal_power_refresh_sample();
    if(!furi_hal_power.last_sample_ok) {
        return 0;
    }
    return furi_hal_power.filtered_pct;
}

uint8_t furi_hal_power_get_bat_health_pct(void) {
    if(furi_hal_bq27220_is_present()) {
        return furi_hal_bq27220_get_health_pct();
    }
    return furi_hal_power_gauge_is_ok() ? 100 : 0;
}

bool furi_hal_power_is_charging(void) {
    /* Prefer BQ25896 charger status (like STM32), fallback to BQ27220 */
    if(furi_hal_bq25896_is_present()) {
        return furi_hal_bq25896_is_charging();
    }
    if(furi_hal_bq27220_is_present()) {
        return furi_hal_bq27220_is_charging();
    }
    furi_hal_power_refresh_sample();
    return furi_hal_power_is_usb_present() && (furi_hal_power.suppress_charge == 0);
}

bool furi_hal_power_is_charging_done(void) {
    /* The charger knows when termination happened (CHRG_STAT = 11); the gauge
     * SoC may sit at 99% for a while after that. */
    if(furi_hal_bq25896_is_present()) {
        return furi_hal_bq25896_is_charging_done();
    }
#if FURI_HAL_POWER_HAS_GAUGE
    if(furi_hal_bq27220_is_present()) {
        return furi_hal_bq27220_is_charging() && furi_hal_bq27220_is_full_charged();
    }
#endif
    if(!furi_hal_power.has_battery_reading) {
        return false;
    }
    return furi_hal_power_is_charging() && (furi_hal_power_get_pct() >= 100);
}

static bool furi_hal_power_gpio_is_low(gpio_num_t pin) {
    return gpio_get_level(pin) == 0;
}

/* Shared teardown before either deep sleep or a real power-off: kill the
 * display first, then wait until every wake/shutdown button is released so
 * EXT0 does not fire the instant we sleep. */
static bool furi_hal_power_prepare_shutdown(void) {
    /* Radios first, while the shared SPI and the I2C bus still work (the LCD
     * teardown below turns SCLK/MOSI into plain GPIOs):
     *  - CC1101: SIDLE, GDO pins to 0x2F, SPWD -> SLEEP, 0.2 uA typ (SWRS061I).
     *  - PN532: PowerDown, soft power-down 22 uA typ instead of several mA. */
    furi_hal_subghz_sleep();
    furi_hal_nfc_power_down();

    /* Stop notification output before blanking the panel. Send the final
     * black frame before locking out future WS2812/RMT updates. */
    furi_hal_light_blink_stop();
    const bool ws2812_black_sent = furi_hal_light_shutdown_ws2812();
    if(!ws2812_black_sent) {
        ESP_LOGW(TAG, "WS2812 black frame was not confirmed");
    }
    /* Beri waktu frame nol WS2812 + reset latch (>280 us) selesai SEBELUM pin
     * diambil alih GPIO; kalau tidak frame terpotong dan LED menahan warna lama. */
    esp_rom_delay_us(2000);
#ifdef BOARD_PIN_WS2812_DATA
    gpio_set_direction((gpio_num_t)BOARD_PIN_WS2812_DATA, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode((gpio_num_t)BOARD_PIN_WS2812_DATA, GPIO_FLOATING);
    gpio_set_level((gpio_num_t)BOARD_PIN_WS2812_DATA, 0);
    ESP_LOGI(TAG, "WS2812 shutdown: GPIO%d driven LOW (level=%d)",
        BOARD_PIN_WS2812_DATA,
        gpio_get_level((gpio_num_t)BOARD_PIN_WS2812_DATA));
#endif
#ifdef BOARD_PIN_LCD_BL
    furi_hal_display_set_backlight(0);
    gpio_set_direction((gpio_num_t)BOARD_PIN_LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)BOARD_PIN_LCD_BL, BOARD_LCD_BL_ACTIVE_LOW ? 1 : 0);
#endif
    furi_hal_display_sleep();

    /* Leave the panel and its SPI pins in a quiet, known state. This prevents
     * a retained PWM/SPI level from keeping the TFT visibly biased during
     * deep sleep. */
#ifdef BOARD_PIN_LCD_RST
    gpio_set_direction((gpio_num_t)BOARD_PIN_LCD_RST, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)BOARD_PIN_LCD_RST, 0);
#endif
#ifdef BOARD_PIN_LCD_DC
    gpio_set_direction((gpio_num_t)BOARD_PIN_LCD_DC, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)BOARD_PIN_LCD_DC, 0);
#endif
#ifdef BOARD_PIN_LCD_CS
    gpio_set_direction((gpio_num_t)BOARD_PIN_LCD_CS, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)BOARD_PIN_LCD_CS, 1);
#endif
#ifdef BOARD_PIN_LCD_MOSI
    gpio_set_direction((gpio_num_t)BOARD_PIN_LCD_MOSI, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)BOARD_PIN_LCD_MOSI, 0);
#endif
#ifdef BOARD_PIN_LCD_SCLK
    gpio_set_direction((gpio_num_t)BOARD_PIN_LCD_SCLK, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)BOARD_PIN_LCD_SCLK, 0);
#endif

    /* Power down peripherals (CC1101 + WS2812) */
#ifdef BOARD_PIN_PWR_EN
    gpio_set_level((gpio_num_t)BOARD_PIN_PWR_EN, 0);
#endif

    /* Select/BOOT (GPIO0) is the deep-sleep wake pin. The Up button is not
     * waited for because it intentionally remains held when long-press
     * shutdown is requested. */
    const TickType_t wait_slice = pdMS_TO_TICKS(10);
    const uint32_t max_wait_ms = 5000;
    uint32_t waited_ms = 0;
    bool qon_held_after_wait = false;
    for(;;) {
        bool boot_held = furi_hal_power_gpio_is_low((gpio_num_t)BOARD_PIN_BUTTON_BOOT);
#ifdef BOARD_PIN_BQ25896_QON
#ifdef BOARD_PIN_BQ25896_QON_ACTIVE_HIGH
        bool qon_held = !furi_hal_power_gpio_is_low((gpio_num_t)BOARD_PIN_BQ25896_QON);
#else
        bool qon_held = furi_hal_power_gpio_is_low((gpio_num_t)BOARD_PIN_BQ25896_QON);
#endif
#else
        bool qon_held = false;
#endif
        bool shutdown_held = false;
#ifdef BOARD_SHUTDOWN_BUTTON
#ifdef BOARD_SHUTDOWN_BUTTON_ACTIVE_HIGH
        shutdown_held = !furi_hal_power_gpio_is_low((gpio_num_t)BOARD_SHUTDOWN_BUTTON);
#else
        shutdown_held = furi_hal_power_gpio_is_low((gpio_num_t)BOARD_SHUTDOWN_BUTTON);
#endif
#endif
        if(!boot_held && !qon_held && !shutdown_held) break;
        if(waited_ms >= max_wait_ms) {
            ESP_LOGE(
                TAG,
                "Shutdown input still held (boot=%d qon=%d shutdown=%d); continuing safely",
                boot_held,
                qon_held,
                shutdown_held);
            qon_held_after_wait = qon_held;
            break;
        }
        vTaskDelay(wait_slice);
        waited_ms += 10;
    }
    vTaskDelay(pdMS_TO_TICKS(300)); /* clear QON wake-detect window */
    if(qon_held_after_wait) {
        ESP_LOGE(TAG, "Shutdown aborted: BQ25896 /QON is still asserted");
        return false;
    }
    ESP_LOGI(TAG, "Shutdown inputs released; continuing to power-off decision");
    return true;
}

/* ========================================================================
 * Deep-sleep GPIO isolation
 *
 * Aturan:
 *  - Tabel di bawah adalah SATU-SATUNYA sumber kebenaran state pin saat deep sleep.
 *  - Output -> di-drive ke level idle lalu di-latch (hold). Input tak terpakai
 *    tanpa bias eksternal -> pull-up/down lalu di-latch. Tidak ada yang floating.
 *  - Pin wake (BOOT) dan pin ber-bias eksternal (mode External) TIDAK disentuh:
 *    konfigurasi wake tetap utuh, tidak ada hold.
 *  - Pin di rail yang mati oleh PWR_EN harus LOW (bukan HIGH), kalau tidak arus
 *    bocor lewat dioda proteksi periferal (back-feed) dan rail ikut setengah hidup.
 *  - Semua pin GPIO yang tidak ada di tabel dilaporkan oleh audit saat shutdown.
 * ======================================================================== */

typedef enum {
    FuriHalPowerPinLow,      /* output LOW, latched */
    FuriHalPowerPinHigh,     /* output HIGH, latched */
    FuriHalPowerPinPullUp,   /* input + pull-up, latched (input tak terpakai / bus idle-high) */
    FuriHalPowerPinPullDown, /* input + pull-down, latched (mis. output periferal yang mati) */
    FuriHalPowerPinExternal, /* bias eksternal / wake: JANGAN disentuh, tanpa hold */
} FuriHalPowerPinMode;

typedef struct {
    gpio_num_t pin;
    FuriHalPowerPinMode mode;
} FuriHalPowerPinEntry;

/* Pin tambahan milik board (CC1101 CS/SCK/MOSI/GDO0/GDO2, SD, NFC, IR, tombol lain, ...).
 * Definisikan di board.h atau lewat -D, dipisah koma dan diakhiri koma. Contoh:
 *   #define FURI_HAL_POWER_EXTRA_PINS \
 *       {GPIO_NUM_10, FuriHalPowerPinLow}, {GPIO_NUM_11, FuriHalPowerPinPullDown},
 * Pin di rail yang mati oleh PWR_EN -> Low (output) atau PullDown (input dari periferal). */
#ifndef FURI_HAL_POWER_EXTRA_PINS
#define FURI_HAL_POWER_EXTRA_PINS
#endif

/* Pin yang dikecualikan dari audit "unmanaged": flash/PSRAM SPI dan USB PHY. */
#ifndef FURI_HAL_POWER_AUDIT_SKIP_MASK
#if CONFIG_IDF_TARGET_ESP32S3
#define FURI_HAL_POWER_AUDIT_SKIP_MASK \
    ((1ULL << 19) | (1ULL << 20) | (((1ULL << 12) - 1ULL) << 26))
#else
#define FURI_HAL_POWER_AUDIT_SKIP_MASK 0ULL
#endif
#endif

#if SOC_GPIO_SUPPORT_HOLD_IO_IN_DSLP && !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
#define FURI_HAL_POWER_GLOBAL_DSLP_HOLD 1
#else
#define FURI_HAL_POWER_GLOBAL_DSLP_HOLD 0
#endif

static const FuriHalPowerPinEntry furi_hal_power_pins[] = {
    /* Wake source: BOOT punya pull-up eksternal. Tidak di-hold, config RTC dijaga. */
    {(gpio_num_t)BOARD_PIN_BUTTON_BOOT, FuriHalPowerPinExternal},
#ifdef BOARD_PIN_BQ25896_QON
    /* Net /QON di-bias oleh BQ25896. Jangan di-drive atau di-pull dari ESP32. */
    {(gpio_num_t)BOARD_PIN_BQ25896_QON, FuriHalPowerPinExternal},
#endif
#if defined(BOARD_SHUTDOWN_BUTTON) && \
    (!defined(BOARD_PIN_BQ25896_QON) || BOARD_SHUTDOWN_BUTTON != BOARD_PIN_BQ25896_QON)
    /* Bukan wake source. Pull-up di-latch supaya tidak floating (redundan bila ada pull-up eksternal). */
    {(gpio_num_t)BOARD_SHUTDOWN_BUTTON, FuriHalPowerPinPullUp},
#endif
#ifdef BOARD_PIN_PWR_EN
    {(gpio_num_t)BOARD_PIN_PWR_EN, FuriHalPowerPinLow},
#endif
#ifdef BOARD_PIN_WS2812_DATA
    {(gpio_num_t)BOARD_PIN_WS2812_DATA, FuriHalPowerPinLow},
#endif
#ifdef BOARD_PIN_LCD_BL
    {(gpio_num_t)BOARD_PIN_LCD_BL, BOARD_LCD_BL_ACTIVE_LOW ? FuriHalPowerPinHigh : FuriHalPowerPinLow},
#endif
#ifdef BOARD_PIN_LCD_RST
    {(gpio_num_t)BOARD_PIN_LCD_RST, FuriHalPowerPinLow},
#endif
#ifdef BOARD_PIN_LCD_DC
    {(gpio_num_t)BOARD_PIN_LCD_DC, FuriHalPowerPinLow},
#endif
#ifdef BOARD_PIN_LCD_CS
    /* HIGH = deselect. Ganti ke Low bila panel LCD ada di rail yang dimatikan PWR_EN. */
    {(gpio_num_t)BOARD_PIN_LCD_CS, FuriHalPowerPinHigh},
#endif
#ifdef BOARD_PIN_LCD_MOSI
    {(gpio_num_t)BOARD_PIN_LCD_MOSI, FuriHalPowerPinLow},
#endif
#ifdef BOARD_PIN_LCD_SCLK
    {(gpio_num_t)BOARD_PIN_LCD_SCLK, FuriHalPowerPinLow},
#endif
#if defined(BOARD_PIN_QWIIC_SDA) && defined(BOARD_PIN_QWIIC_SCL)
    /* I2C idle-high lewat pull-up, BUKAN push-pull: slave yang menahan SDA rendah
     * tidak akan berbenturan dengan output ESP32, dan bus tetap terdefinisi. */
    {(gpio_num_t)BOARD_PIN_QWIIC_SDA, FuriHalPowerPinPullUp},
    {(gpio_num_t)BOARD_PIN_QWIIC_SCL, FuriHalPowerPinPullUp},
#endif
    FURI_HAL_POWER_EXTRA_PINS
};

/* Pin analog/wake yang tidak boleh di-drive, di-pull, maupun di-hold. */
static bool furi_hal_power_pin_is_reserved(gpio_num_t pin) {
    if(pin == (gpio_num_t)BOARD_PIN_BUTTON_BOOT) return true; /* wake */
    if((int)pin == (int)gpio_battery_sense.pin) return true;  /* divider VBAT -> ADC */
    return false;
}

static bool furi_hal_power_pin_is_managed(gpio_num_t pin) {
    if(furi_hal_power_pin_is_reserved(pin)) return true;
    for(size_t i = 0; i < COUNT_OF(furi_hal_power_pins); i++) {
        if(furi_hal_power_pins[i].pin == pin) return true;
    }
    return false;
}

static bool furi_hal_power_holds_released = false;

/* Lepas hold sisa deep sleep dan serahkan pin ke HAL TANPA glitch: register digital
 * di-preload dengan level/pull yang sama SEBELUM hold dilepas, jadi pad tidak pernah
 * floating di antara "dilepas" dan "dikonfigurasi HAL". Bit hold selamat dari reset,
 * jadi ini WAJIB jalan sebelum HAL display/light/SPI/I2C menyentuh pin.
 * Dipanggil otomatis oleh constructor boot awal; aman dipanggil ulang. */
void furi_hal_power_release_gpio_holds(void) {
    if(furi_hal_power_holds_released) return;
    furi_hal_power_holds_released = true;

#if FURI_HAL_POWER_GLOBAL_DSLP_HOLD
    gpio_deep_sleep_hold_dis();
#endif

    for(size_t i = 0; i < COUNT_OF(furi_hal_power_pins); i++) {
        const gpio_num_t pin = furi_hal_power_pins[i].pin;
        const FuriHalPowerPinMode mode = furi_hal_power_pins[i].mode;
        if(!furi_hal_power_pin_number_valid(pin)) continue;
        if(mode == FuriHalPowerPinExternal || furi_hal_power_pin_is_reserved(pin)) continue;

        switch(mode) {
        case FuriHalPowerPinLow:
        case FuriHalPowerPinHigh:
            gpio_set_level(pin, mode == FuriHalPowerPinHigh ? 1 : 0);
            gpio_set_direction(pin, GPIO_MODE_OUTPUT);
            break;
        case FuriHalPowerPinPullUp:
            gpio_set_direction(pin, GPIO_MODE_INPUT);
            gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
            break;
        case FuriHalPowerPinPullDown:
            gpio_set_direction(pin, GPIO_MODE_INPUT);
            gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
            break;
        default: break;
        }

        if(rtc_gpio_is_valid_gpio(pin)) {
            rtc_gpio_hold_dis(pin);
            rtc_gpio_deinit(pin); /* mux kembali ke fungsi digital */
        } else {
            gpio_hold_dis(pin);
        }
    }

    /* Wake pin: kembalikan mux ke digital (hold memang tidak pernah dipasang). */
    if(rtc_gpio_is_valid_gpio((gpio_num_t)BOARD_PIN_BUTTON_BOOT)) {
        rtc_gpio_deinit((gpio_num_t)BOARD_PIN_BUTTON_BOOT);
    }
}

/* Jalan sebelum app_main (setelah init BSS), jadi hold terlepas sebelum HAL apa pun.
 * Tanpa logging: belum aman di fase ini. Set FURI_HAL_POWER_EARLY_RELEASE=0 untuk
 * mematikannya; furi_hal_power_init() tetap memanggil release (lebih lambat). */
#ifndef FURI_HAL_POWER_EARLY_RELEASE
#define FURI_HAL_POWER_EARLY_RELEASE 1
#endif
#if FURI_HAL_POWER_EARLY_RELEASE
__attribute__((constructor(101))) static void furi_hal_power_early_release(void) {
    furi_hal_power_release_gpio_holds();
}
#endif

/* Drive + latch semua pin di tabel. Return true bila ada pad non-RTC (butuh hold digital global). */
static bool furi_hal_power_isolate_pins(void) {
    bool need_digital_hold = false;
    unsigned held = 0;

    for(size_t i = 0; i < COUNT_OF(furi_hal_power_pins); i++) {
        const gpio_num_t pin = furi_hal_power_pins[i].pin;
        const FuriHalPowerPinMode mode = furi_hal_power_pins[i].mode;
        if(!furi_hal_power_pin_number_valid(pin)) continue;
        if(mode == FuriHalPowerPinExternal || furi_hal_power_pin_is_reserved(pin)) continue;

        if(rtc_gpio_is_valid_gpio(pin)) {
            /* Pad RTC: hold per-pad, tidak memicu isolasi digital global. */
            switch(mode) {
            case FuriHalPowerPinLow:
            case FuriHalPowerPinHigh:
                rtc_gpio_set_level(pin, mode == FuriHalPowerPinHigh ? 1 : 0); /* level dulu: tanpa glitch */
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
            default: continue;
            }
            rtc_gpio_hold_en(pin);
        } else {
            /* Pad digital: butuh gpio_deep_sleep_hold_en() + stack internal (lihat trampoline). */
            switch(mode) {
            case FuriHalPowerPinLow:
            case FuriHalPowerPinHigh:
                gpio_set_direction(pin, GPIO_MODE_OUTPUT);
                gpio_set_level(pin, mode == FuriHalPowerPinHigh ? 1 : 0);
                break;
            case FuriHalPowerPinPullUp:
                gpio_set_direction(pin, GPIO_MODE_INPUT);
                gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
                break;
            case FuriHalPowerPinPullDown:
                gpio_set_direction(pin, GPIO_MODE_INPUT);
                gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
                break;
            default: continue;
            }
            gpio_hold_en(pin);
            need_digital_hold = true;
        }
        held++;
    }

    ESP_LOGI(
        TAG,
        "Deep-sleep pin isolation: %u pin held (digital hold: %d)",
        held,
        (int)need_digital_hold);
    return need_digital_hold;
}

/* Laporkan GPIO yang belum ada di tabel: itulah kandidat pin floating / bocor arus. */
static void furi_hal_power_audit_pins(void) {
    char list[160] = {0};
    size_t len = 0;
    bool truncated = false;

    for(int n = 0; n < (int)GPIO_NUM_MAX; n++) {
        if(!GPIO_IS_VALID_GPIO(n)) continue;
        if((FURI_HAL_POWER_AUDIT_SKIP_MASK >> n) & 1ULL) continue;
        if(furi_hal_power_pin_is_managed((gpio_num_t)n)) continue;
        const int w = snprintf(list + len, sizeof(list) - len, "%d ", n);
        if(w < 0 || (size_t)w >= sizeof(list) - len) {
            truncated = true;
            break;
        }
        len += (size_t)w;
    }

    if(len > 0) {
        ESP_LOGW(
            TAG,
            "Unmanaged GPIO (bisa floating saat deep sleep): %s%s",
            list,
            truncated ? "..." : "");
    } else {
        ESP_LOGI(TAG, "GPIO audit: semua pin terkelola");
    }
}

static volatile bool furi_hal_power_dslp_digital_hold = false;

/* Berjalan di task dengan stack INTERNAL RAM: esp_deep_sleep_start() meng-assert
 * "stack must be in internal ram" bila ada hold digital aktif, sedangkan stack
 * power-service ada di PSRAM. Tidak pernah return. */
static void furi_hal_power_deep_sleep_task(void* arg) {
    UNUSED(arg);
#if FURI_HAL_POWER_GLOBAL_DSLP_HOLD
    if(furi_hal_power_dslp_digital_hold) {
        gpio_deep_sleep_hold_en();
    }
#endif
    esp_deep_sleep_start();
    for(;;) {
    }
}

static void furi_hal_power_start_deep_sleep(bool digital_hold) {
    furi_hal_power_dslp_digital_hold = digital_hold;
    if(xTaskCreateWithCaps(
           furi_hal_power_deep_sleep_task,
           "dsleep",
           4096,
           NULL,
           configMAX_PRIORITIES - 1,
           NULL,
           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == pdPASS) {
        for(;;) {
            vTaskDelay(portMAX_DELAY);
        }
    }
    /* Fallback aman: TANPA hold digital global, supaya tidak assert/bootloop. */
    ESP_LOGE(TAG, "Internal-stack sleep task failed; digital pad hold skipped");
    esp_deep_sleep_start();
}

/* Enter ESP32 deep sleep, waking on the BOOT/encoder button. The RTC domain
 * stays powered (~µA draw); this is not a true power cut. Does not return. */
static void furi_hal_power_enter_deep_sleep(
    bool wake_on_boot_button,
    bool usb_watch,
    uint32_t usb_watch_interval_s) {
    esp_err_t disable_err = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if(disable_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to clear previous wake sources: %s", esp_err_to_name(disable_err));
    }
    ESP_LOGI(TAG, "Deep-sleep wake sources cleared");

    if(wake_on_boot_button) {
        const gpio_num_t wake_pin = (gpio_num_t)BOARD_PIN_BUTTON_BOOT;

        if(furi_hal_power_gpio_is_low(wake_pin)) {
            ESP_LOGW(TAG, "Wake pin GPIO%d still LOW: deep sleep will wake immediately", (int)wake_pin);
        }

        /* Digital pull-ups die in deep sleep. BOOT punya pull-up eksternal; pull-up RTC
         * ini redundan tapi tidak berbenturan (keduanya menarik ke 3V3) dan menjaga level
         * idle bila pull-up eksternal terputus. Wake pin TIDAK boleh di-hold. */
        if(!rtc_gpio_is_valid_gpio(wake_pin)) {
            ESP_LOGE(
                TAG,
                "Wake pin GPIO%d is not RTC-capable; deep-sleep wake is unreliable",
                (int)wake_pin);
        } else {
            rtc_gpio_init(wake_pin);
            rtc_gpio_set_direction(wake_pin, RTC_GPIO_MODE_INPUT_ONLY);
            rtc_gpio_pullup_en(wake_pin);
            rtc_gpio_pulldown_dis(wake_pin);
        }

        /* Wake on the BOOT/encoder/Select button (active low). */
        esp_err_t wake_err;
#if SOC_PM_SUPPORT_EXT0_WAKEUP
        wake_err = esp_sleep_enable_ext0_wakeup(wake_pin, 0);
#else
        wake_err = esp_deep_sleep_enable_gpio_wakeup(BIT(wake_pin), ESP_GPIO_WAKEUP_GPIO_LOW);
#endif
        if(wake_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure shutdown wake source: %s", esp_err_to_name(wake_err));
        } else {
            ESP_LOGI(TAG, "Deep-sleep wake source: GPIO%d", (int)wake_pin);
        }
    } else {
        ESP_LOGI(TAG, "Deep-sleep wake source: none (battery shutdown path)");
    }

    /* Bersihkan bit auto-hold global (RTC_CNTL_DIG_ISO) sisa firmware lama; bit ini
     * selamat dari reset/panic. Hold sekarang dipasang PER-PAD di bawah, dan hold digital
     * global hanya diaktifkan dari task ber-stack internal. Tidak ada di C6
     * (SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP=1) sehingga build C6 tetap link. */
#if FURI_HAL_POWER_GLOBAL_DSLP_HOLD
    gpio_deep_sleep_hold_dis();
#endif

    /* 1) Charger: HARUS sebelum pin I2C di-hold. Menjamin baterai tetap ter-charge otonom
     *    saat MCU tidur, walau USB/OTG dicabut lalu dipasang lagi. */
    if(furi_hal_bq25896_is_present() && !furi_hal_bq25896_prepare_for_sleep()) {
        ESP_LOGW(TAG, "BQ25896 sleep prep not verified; charging in deep sleep uncertain");
    }

    /* 2) Audit lalu isolasi seluruh pin. */
    furi_hal_power_audit_pins();
    const bool digital_hold = furi_hal_power_isolate_pins();

    /* 3) Pull/level RTC IO dan konfigurasi wake tetap hidup selama tidur. */
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    /* 4. On USB power, optionally wake periodically to check whether USB is
     *    still connected. If enabled and USB was removed, disconnect the
     *    battery instead of feeding always-on peripherals for days. */
    const bool usb_watch_enabled = usb_watch_interval_s || furi_hal_power_usb_dslp_check_enabled;
    const uint32_t check_interval_s =
        usb_watch_interval_s ? usb_watch_interval_s : furi_hal_power_usb_dslp_check_interval_s;
    if(usb_watch && usb_watch_enabled && check_interval_s >= FURI_HAL_POWER_USB_DSLP_MIN_CHECK_S &&
       check_interval_s <= FURI_HAL_POWER_USB_DSLP_MAX_CHECK_S &&
       esp_sleep_enable_timer_wakeup((uint64_t)check_interval_s * 1000000ULL) == ESP_OK) {
        furi_hal_power_usb_dslp_magic = FURI_HAL_POWER_USB_DSLP_MAGIC;
        furi_hal_power_usb_dslp_interval_s = check_interval_s;
        ESP_LOGI(TAG, "Deep-sleep USB check every %us", (unsigned)check_interval_s);
    } else {
        furi_hal_power_usb_dslp_magic = 0;
        furi_hal_power_usb_dslp_interval_s = 0;
    }

    ESP_LOGI(TAG, "Entering ESP32 deep sleep");
    furi_hal_power_start_deep_sleep(digital_hold);
}

/* Deep-sleep power-off (default mode). */
void furi_hal_power_shutdown(void) {
    if(!furi_hal_power_prepare_shutdown()) {
        /* Display, LED and peripheral rail are already torn down; staying awake
         * would leave a black, half-initialized device. Restart cleanly. */
        ESP_LOGE(TAG, "Deep-sleep shutdown aborted during hardware preparation; restarting");
        esp_restart();
    }
    furi_hal_power_enter_deep_sleep(true, false, 0);
}

/* Real power-off: put the BQ25896 charger into ship mode (BATFET off), which
 * physically disconnects the battery. Any charger/I2C uncertainty aborts
 * instead of falling back to deep sleep. USB intentionally uses deep sleep. */
bool furi_hal_power_off(void) {
    furi_hal_power_shutdown_in_progress = true;
    const bool charger_present = furi_hal_bq25896_is_present();
    bool vbus_present = false;
    const bool vbus_read_ok =
        charger_present && furi_hal_bq25896_get_vbus_present(&vbus_present);
    ESP_LOGI(
        TAG,
        "Power off requested: charger=%d vbus=%d vbus_read_ok=%d",
        charger_present,
        vbus_present,
        vbus_read_ok);

    if(!charger_present) {
        ESP_LOGE(TAG, "Power-off aborted: BQ25896 is not available");
        furi_hal_power_shutdown_in_progress = false;
        return false;
    }
    if(!vbus_read_ok) {
        ESP_LOGE(TAG, "Power-off aborted: VBUS state is unknown");
        furi_hal_power_shutdown_in_progress = false;
        return false;
    }

    if(!vbus_present) {
        bool ship_mode = furi_hal_bq25896_poweroff();
        if(!ship_mode) {
            ESP_LOGW(
                TAG,
                "BQ25896 BATFET ship-mode request failed; refusing deep sleep");
            ESP_LOGE(TAG, "Power-off aborted: hardware shutdown was not confirmed");
            furi_hal_power_shutdown_in_progress = false;
            return false;
        }
        ESP_LOGI(TAG, "BQ25896 BATFET ship-mode command confirmed");
        /* The charger should remove SYS immediately. Do not configure an
         * ESP32 wake source or continue into deep sleep from this path. */
        return true;
    }

    /* USB keeps SYS powered, so use the intentional USB deep-sleep path. */
    ESP_LOGI(TAG, "USB power detected; entering deep sleep with GPIO0 wake");
    if(!furi_hal_power_prepare_shutdown()) {
        /* See furi_hal_power_shutdown(): the HAL is torn down at this point. */
        ESP_LOGE(TAG, "Power-off aborted during hardware preparation; restarting");
        esp_restart();
    }
#if defined(BOARD_HAS_BQ25896_SHIP_MODE) && BOARD_HAS_BQ25896_SHIP_MODE
    furi_hal_power_enter_deep_sleep(true, true, 0);
#else
    furi_hal_power_enter_deep_sleep(true, false, 0);
#endif
    return true;
}

/* Runs from furi_hal_power_init() after the charger is probed. A timer wake-up
 * with the marker set means the device was shut down on USB power and is only
 * checking whether USB is still connected: no display, no services. */
static void furi_hal_power_usb_deep_sleep_check(void) {
    if(furi_hal_power_usb_dslp_magic != FURI_HAL_POWER_USB_DSLP_MAGIC) return;
    furi_hal_power_usb_dslp_magic = 0;
    uint32_t check_interval_s = furi_hal_power_usb_dslp_interval_s;
    if(check_interval_s < FURI_HAL_POWER_USB_DSLP_MIN_CHECK_S ||
       check_interval_s > FURI_HAL_POWER_USB_DSLP_MAX_CHECK_S) {
        check_interval_s = FURI_HAL_POWER_USB_DSLP_CHECK_S;
    }
    if(esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) return; /* user wake */
    if(furi_hal_power_gpio_is_low((gpio_num_t)BOARD_PIN_BUTTON_BOOT)) return; /* OK pressed */

    bool vbus_present = true;
    const bool read_ok =
        furi_hal_bq25896_is_present() && furi_hal_bq25896_get_vbus_present(&vbus_present);
    if(read_ok && !vbus_present) {
#if defined(BOARD_HAS_BQ25896_SHIP_MODE) && BOARD_HAS_BQ25896_SHIP_MODE
        ESP_LOGW(TAG, "USB removed while in deep sleep: disconnecting battery (ship mode)");
        furi_hal_power_shutdown_in_progress = true;
        (void)furi_hal_bq25896_poweroff(); /* does not return when the battery is cut */
        furi_hal_power_shutdown_in_progress = false;
        ESP_LOGE(TAG, "Ship mode not confirmed; returning to deep sleep");
#endif
    } else {
        ESP_LOGI(TAG, "Deep-sleep USB check: %s", read_ok ? "USB present" : "VBUS unknown");
    }
    furi_hal_power_enter_deep_sleep(true, true, check_interval_s);
}

FURI_NORETURN void furi_hal_power_reset(void) {
    esp_restart();
    __builtin_unreachable();
}

bool furi_hal_power_enable_otg(void) {
    if(furi_hal_power_shutdown_in_progress) {
        ESP_LOGW(TAG, "OTG enable rejected while shutdown is in progress");
        return false;
    }
    if(furi_hal_bq25896_is_present()) {
        furi_hal_bq25896_enable_otg();
        furi_hal_power.otg_enabled = furi_hal_bq25896_is_otg_enabled();
        return furi_hal_power.otg_enabled;
    }
    furi_hal_power.otg_enabled = true;
    return true;
}

void furi_hal_power_disable_otg(void) {
    if(furi_hal_bq25896_is_present()) {
        furi_hal_bq25896_disable_otg();
    }
    furi_hal_power.otg_enabled = false;
}

bool furi_hal_power_check_otg_fault(void) {
    return false;
}

void furi_hal_power_check_otg_status(void) {
}

bool furi_hal_power_is_otg_enabled(void) {
    if(furi_hal_bq25896_is_present()) {
        furi_hal_power.otg_enabled = furi_hal_bq25896_is_otg_enabled();
    }
    return furi_hal_power.otg_enabled;
}

float furi_hal_power_get_battery_charge_voltage_limit(void) {
    if(furi_hal_bq25896_is_present()) {
        return (float)furi_hal_bq25896_get_vreg_voltage_mv() / 1000.0f;
    }
    return furi_hal_power.charge_voltage_limit;
}

void furi_hal_power_set_battery_charge_voltage_limit(float voltage) {
    furi_hal_power.charge_voltage_limit = furi_hal_power_quantize_charge_limit(voltage);
    if(furi_hal_bq25896_is_present()) {
        /* Pass the quantized value in rounded mV: (uint16_t)(4.192f * 1000)
         * truncated to 4191 mV, one 16 mV VREG step below the request. */
        furi_hal_bq25896_set_vreg_voltage_mv(
            (uint16_t)lroundf(furi_hal_power.charge_voltage_limit * 1000.0f));
    }
}

uint32_t furi_hal_power_get_battery_remaining_capacity(void) {
    if(furi_hal_bq27220_is_present()) {
        return furi_hal_bq27220_get_remaining_capacity_mah();
    }
    return (uint32_t)((furi_hal_power_get_pct() * FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH) / 100U);
}

uint32_t furi_hal_power_get_battery_full_capacity(void) {
    if(furi_hal_bq27220_is_present()) {
        return furi_hal_bq27220_get_full_charge_capacity_mah();
    }
    return FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH;
}

uint32_t furi_hal_power_get_battery_design_capacity(void) {
    if(furi_hal_bq27220_is_present()) {
        return furi_hal_bq27220_get_design_capacity_mah();
    }
    return FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH;
}

float furi_hal_power_get_battery_voltage(FuriHalPowerIC ic) {
#if defined(BOARD_HAS_BQ27220) && !BOARD_HAS_BQ27220
    if(ic == FuriHalPowerICFuelGauge) {
        furi_hal_power_refresh_sample();
        return furi_hal_power_get_estimated_battery_voltage();
    }
#endif
    if(ic == FuriHalPowerICCharger && furi_hal_bq25896_is_present()) {
        return (float)furi_hal_bq25896_get_vbat_voltage_mv() / 1000.0f;
    }
    if(ic == FuriHalPowerICFuelGauge && furi_hal_bq27220_is_present()) {
        return (float)furi_hal_bq27220_get_voltage_mv() / 1000.0f;
    }
    /* Fallback: try whichever is available */
    if(furi_hal_bq27220_is_present()) {
        return (float)furi_hal_bq27220_get_voltage_mv() / 1000.0f;
    }
    if(furi_hal_bq25896_is_present()) {
        return (float)furi_hal_bq25896_get_vbat_voltage_mv() / 1000.0f;
    }
    return furi_hal_power_get_estimated_battery_voltage();
}

float furi_hal_power_get_battery_current(FuriHalPowerIC ic) {
    if(ic == FuriHalPowerICCharger && furi_hal_bq25896_is_present()) {
        return (float)furi_hal_bq25896_get_vbat_current_ma() / 1000.0f;
    }
    if(ic == FuriHalPowerICFuelGauge && furi_hal_bq27220_is_present()) {
        return (float)furi_hal_bq27220_get_current_ma() / 1000.0f;
    }
    if(furi_hal_bq27220_is_present()) {
        return (float)furi_hal_bq27220_get_current_ma() / 1000.0f;
    }
    return 0.0f;
}

float furi_hal_power_get_battery_temperature(FuriHalPowerIC ic) {
    if(ic == FuriHalPowerICCharger && furi_hal_bq25896_is_present()) {
        return (float)furi_hal_bq25896_get_temperature_mc() / 1000.0f;
    }
    if(ic == FuriHalPowerICFuelGauge && furi_hal_bq27220_is_present()) {
        /* BQ27220 returns 0.1°K, convert to °C: (raw - 2731) / 10 */
        uint16_t raw = furi_hal_bq27220_get_temperature_raw();
        return ((float)raw - 2731.0f) / 10.0f;
    }
    /* Fallback */
    if(furi_hal_bq27220_is_present()) {
        uint16_t raw = furi_hal_bq27220_get_temperature_raw();
        return ((float)raw - 2731.0f) / 10.0f;
    }
    if(furi_hal_bq25896_is_present()) {
        return (float)furi_hal_bq25896_get_temperature_mc() / 1000.0f;
    }
    return 25.0f;
}

float furi_hal_power_get_usb_voltage(void) {
    if(furi_hal_bq25896_is_present()) {
        /* VBUS presence from the cached VBUS_GD status; the ADC register is only
         * read when USB is there (it is stale while the ADC is off). */
        if(!furi_hal_power_is_usb_connected()) return 0.0f;
        return (float)furi_hal_bq25896_get_vbus_voltage_mv() / 1000.0f;
    }
    furi_hal_power_refresh_sample();
    return furi_hal_power_is_usb_present() ? furi_hal_power.last_supply_voltage : 0.0f;
}

void furi_hal_power_enable_external_3_3v(void) {
}

void furi_hal_power_disable_external_3_3v(void) {
}

/* Same contract as the STM32 HAL: the first enter disables charging in the
 * charger, the last exit enables it again (charge limit setting, Sub-GHz). */
void furi_hal_power_suppress_charge_enter(void) {
    FURI_CRITICAL_ENTER();
    const bool first = furi_hal_power.suppress_charge == 0;
    if(furi_hal_power.suppress_charge < UINT8_MAX) {
        furi_hal_power.suppress_charge++;
    }
    FURI_CRITICAL_EXIT();
    if(first && furi_hal_bq25896_is_present()) {
        furi_hal_bq25896_disable_charging();
    }
}

void furi_hal_power_suppress_charge_exit(void) {
    FURI_CRITICAL_ENTER();
    const bool was_suppressed = furi_hal_power.suppress_charge > 0;
    if(was_suppressed) {
        furi_hal_power.suppress_charge--;
    }
    const bool last = was_suppressed && furi_hal_power.suppress_charge == 0;
    FURI_CRITICAL_EXIT();
    if(last && furi_hal_bq25896_is_present()) {
        furi_hal_bq25896_enable_charging();
    }
}

void furi_hal_power_info_get(PropertyValueCallback out, char sep, void* context) {
    furi_check(out);

    FuriString* key = furi_string_alloc();
    FuriString* value = furi_string_alloc();
    PropertyValueContext property_context = {
        .key = key,
        .value = value,
        .out = out,
        .sep = sep,
        .last = false,
        .context = context,
    };

    property_value_out(&property_context, NULL, 2, "format", "major", "2");
    property_value_out(&property_context, NULL, 2, "format", "minor", "0");
    property_value_out(&property_context, "%u", 2, "charge", "level", furi_hal_power_get_pct());
    property_value_out(
        &property_context,
        NULL,
        2,
        "charge",
        "state",
        furi_hal_power_is_charging() ?
            (furi_hal_power_is_charging_done() ? "charged" : "charging") :
            "discharging");
    property_value_out(
        &property_context,
        "%u",
        3,
        "charge",
        "voltage",
        "limit",
        (unsigned int)lroundf(furi_hal_power_get_battery_charge_voltage_limit() * 1000.0f));
    property_value_out(
        &property_context,
        "%u",
        2,
        "battery",
        "voltage",
        (unsigned int)lroundf(
            furi_hal_power_get_battery_voltage(FuriHalPowerICFuelGauge) * 1000.0f));
    property_value_out(
        &property_context,
        "%u",
        2,
        "battery",
        "health",
        furi_hal_power_get_bat_health_pct());
    property_context.last = true;
    property_value_out(
        &property_context,
        "%lu",
        2,
        "capacity",
        "remain",
        (unsigned long)furi_hal_power_get_battery_remaining_capacity());

    furi_string_free(key);
    furi_string_free(value);
}

void furi_hal_power_debug_get(PropertyValueCallback out, void* context) {
    furi_check(out);

    FuriString* key = furi_string_alloc();
    FuriString* value = furi_string_alloc();
    PropertyValueContext property_context = {
        .key = key,
        .value = value,
        .out = out,
        .sep = '.',
        .last = false,
        .context = context,
    };

    property_value_out(&property_context, NULL, 2, "format", "major", "1");
    property_value_out(&property_context, NULL, 2, "format", "minor", "1");
    property_value_out(
        &property_context,
        "%u",
        2,
        "adc",
        "gpio",
        (unsigned int)gpio_battery_sense.pin);
    property_value_out(
        &property_context,
        "%u",
        2,
        "adc",
        "ready",
        furi_hal_power_gauge_is_ok() ? 1U : 0U);
    property_value_out(
        &property_context,
        "%d",
        2,
        "adc",
        "raw",
        furi_hal_power.last_adc_raw);
    property_value_out(
        &property_context,
        "%u",
        2,
        "adc",
        "pin_mv",
        (unsigned int)furi_hal_power.last_adc_pin_mv);
    property_value_out(
        &property_context,
        "%u",
        2,
        "adc",
        "vbat_mv",
        (unsigned int)lroundf(furi_hal_power.last_adc_battery_voltage * 1000.0f));
    property_value_out(
        &property_context,
        "%u",
        2,
        "charger",
        "vbat_mv",
        (unsigned int)lroundf(furi_hal_power.last_charger_battery_voltage * 1000.0f));
    property_value_out(
        &property_context,
        NULL,
        2,
        "battery",
        "source",
        furi_hal_power.last_sample_ok ?
            (furi_hal_power.battery_voltage_from_adc ? "adc" : "charger") :
            "invalid");
    property_value_out(
        &property_context,
        "%u",
        2,
        "charge",
        "target_pct",
        furi_hal_power.last_target_pct);
    property_value_out(
        &property_context,
        "%u",
        2,
        "charge",
        "filtered_pct",
        furi_hal_power.filtered_pct);
    property_value_out(
        &property_context,
        "%u",
        2,
        "supply",
        "mv",
        (unsigned int)lroundf(furi_hal_power.last_supply_voltage * 1000.0f));
#if FURI_HAL_POWER_HAS_GAUGE
    FuriHalBq27220Snapshot gauge;
    if(furi_hal_bq27220_is_present() && furi_hal_bq27220_get_snapshot(&gauge)) {
        property_value_out(&property_context, "%u", 2, "gauge", "fresh", gauge.fresh ? 1U : 0U);
        property_value_out(
            &property_context, "%04X", 2, "gauge", "status", (unsigned int)gauge.battery_status);
        property_value_out(
            &property_context, "%04X", 2, "gauge", "opstatus", (unsigned int)gauge.operation_status);
        property_value_out(
            &property_context, "%d", 2, "gauge", "avg_ma", (int)gauge.average_current_ma);
        property_value_out(
            &property_context, "%u", 2, "gauge", "tte_min", (unsigned int)gauge.time_to_empty_min);
        property_value_out(
            &property_context, "%u", 2, "gauge", "ttf_min", (unsigned int)gauge.time_to_full_min);
        property_value_out(
            &property_context, "%u", 2, "gauge", "cycles", (unsigned int)gauge.cycle_count);
        property_value_out(
            &property_context,
            "%u",
            2,
            "gauge",
            "fcc_mah",
            (unsigned int)gauge.full_charge_capacity_mah);
        property_value_out(
            &property_context,
            "%u",
            2,
            "gauge",
            "design_mah",
            (unsigned int)furi_hal_bq27220_get_design_capacity_mah());
    }
#endif
    property_value_out(
        &property_context,
        "%u",
        2,
        "protect",
        "low",
        furi_hal_power_protection.low ? 1U : 0U);
    property_value_out(
        &property_context,
        "%u",
        2,
        "protect",
        "critical",
        furi_hal_power_protection.critical ? 1U : 0U);
    property_value_out(
        &property_context,
        "%u",
        2,
        "lightsleep",
        "active",
        furi_hal_power_is_light_sleep_active() ? 1U : 0U);
    property_context.last = true;
    property_value_out(
        &property_context,
        "%u",
        2,
        "battery",
        "mv",
        (unsigned int)lroundf(furi_hal_power_get_estimated_battery_voltage() * 1000.0f));

    furi_string_free(key);
    furi_string_free(value);
}
