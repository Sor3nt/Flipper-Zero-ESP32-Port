#include "furi_hal_power.h"
#include "furi_hal_bq27220.h"
#include "furi_hal_bq25896.h"
#include "furi_hal_display.h"
#include "boards/board.h"

#include <math.h>
#include <stdio.h>

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
#include <driver/i2c.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <driver/gpio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <furi.h>

#include "furi_hal_resources.h"
#include "furi_hal_light.h"
#include "furi_hal_power_sleep.h"
#include <driver/rtc_io.h>

#define TAG "FuriHalPower"

#if CONFIG_PM_ENABLE
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
/* What the idle logic asked for, and how many sessions veto it. The gate is
 * open only when requested and not inhibited. Both are changed under
 * furi_hal_power_ls_mutex, together with the lock transition they cause. */
static bool furi_hal_power_ls_requested = false;
static uint16_t furi_hal_power_ls_inhibit = 0;
static SemaphoreHandle_t furi_hal_power_ls_mutex = NULL;
static StaticSemaphore_t furi_hal_power_ls_mutex_buffer;
/* Auto Light Sleep: pads parked + WS2812 dark before the gate opens. */
static bool furi_hal_power_ls_safe_mode = false;

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
#define FURI_HAL_POWER_ADC_DIVIDER_RATIO        (3.0f)
#define FURI_HAL_POWER_SAMPLE_REFRESH_US        (250000LL)
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
    int64_t last_sample_us;
    float charge_voltage_limit;
    float last_supply_voltage;
    float last_battery_voltage;
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
    .last_sample_us = 0,
    .charge_voltage_limit = FURI_HAL_POWER_FULL_BATTERY_VOLTAGE_V,
    .last_supply_voltage = 0.0f,
    .last_battery_voltage = FURI_HAL_POWER_FULL_BATTERY_VOLTAGE_V,
};

static FuriPubSub* furi_hal_power_idle_pubsub;
static volatile bool furi_hal_power_idle;

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
    };
    if(adc_cali_create_scheme_line_fitting(&cali_config, out_handle) == ESP_OK) {
        return true;
    }
#endif
    return false;
}

static float furi_hal_power_quantize_charge_limit(float voltage) {
    float clamped = voltage;
    if(clamped < FURI_HAL_POWER_CHARGE_LIMIT_MIN_V) {
        clamped = FURI_HAL_POWER_CHARGE_LIMIT_MIN_V;
    } else if(clamped > FURI_HAL_POWER_CHARGE_LIMIT_MAX_V) {
        clamped = FURI_HAL_POWER_CHARGE_LIMIT_MAX_V;
    }

    const float steps =
        floorf((clamped - FURI_HAL_POWER_CHARGE_LIMIT_MIN_V) / FURI_HAL_POWER_CHARGE_LIMIT_STEP_V);
    return FURI_HAL_POWER_CHARGE_LIMIT_MIN_V + steps * FURI_HAL_POWER_CHARGE_LIMIT_STEP_V;
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
}

static bool furi_hal_power_is_usb_present(void) {
    return furi_hal_power.last_supply_voltage >= FURI_HAL_POWER_USB_PRESENT_THRESHOLD_V;
}

static void furi_hal_power_refresh_sample(void) {
    furi_hal_power_ensure_initialized();
    if(!furi_hal_power.adc_ready) {
        furi_hal_power.last_sample_ok = false;
        return;
    }

    const int64_t now = esp_timer_get_time();
    if((now - furi_hal_power.last_sample_us) < FURI_HAL_POWER_SAMPLE_REFRESH_US &&
       furi_hal_power.last_sample_ok) {
        return;
    }

    int raw_value = 0;
    if(adc_oneshot_read(furi_hal_power.adc_handle, furi_hal_power.adc_channel, &raw_value) != ESP_OK) {
        furi_hal_power.last_sample_ok = false;
        return;
    }

    int pin_mv = 0;
    if(furi_hal_power.adc_cali_ready) {
        if(adc_cali_raw_to_voltage(furi_hal_power.adc_cali_handle, raw_value, &pin_mv) != ESP_OK) {
            furi_hal_power.last_sample_ok = false;
            return;
        }
    } else {
        pin_mv = (raw_value * 3300) / 4095;
    }

    float supply_voltage = ((float)pin_mv / 1000.0f) * FURI_HAL_POWER_ADC_DIVIDER_RATIO;
    if(supply_voltage < 0.0f) {
        supply_voltage = 0.0f;
    } else if(supply_voltage > 6.0f) {
        supply_voltage = 6.0f;
    }

    furi_hal_power.last_supply_voltage = supply_voltage;
    if(!furi_hal_power_is_usb_present()) {
        furi_hal_power.has_battery_reading = true;
        if(supply_voltage < FURI_HAL_POWER_EMPTY_BATTERY_VOLTAGE_V) {
            furi_hal_power.last_battery_voltage = FURI_HAL_POWER_EMPTY_BATTERY_VOLTAGE_V;
        } else if(supply_voltage > furi_hal_power.charge_voltage_limit) {
            furi_hal_power.last_battery_voltage = furi_hal_power.charge_voltage_limit;
        } else {
            furi_hal_power.last_battery_voltage = supply_voltage;
        }
    }

    furi_hal_power.last_sample_us = now;
    furi_hal_power.last_sample_ok = true;
}

static float furi_hal_power_get_estimated_battery_voltage(void) {
    furi_hal_power_refresh_sample();

    if(!furi_hal_power.last_sample_ok) {
        return 0.0f;
    }

    if(furi_hal_power_is_usb_present()) {
        return furi_hal_power.last_battery_voltage;
    }

    return furi_hal_power.last_supply_voltage;
}

void furi_hal_power_init(void) {
    /* Normally already done by the early constructor; idempotent. */
    furi_hal_power_sleep_release_holds();
    furi_hal_power_ensure_initialized();
    if(!furi_hal_power_idle_pubsub) {
        furi_hal_power_idle_pubsub = furi_pubsub_alloc();
    }

#if CONFIG_PM_ENABLE
    /* Dynamic frequency scaling: let the CPU idle at 80 MHz and ramp to 160 MHz
     * only while a peripheral driver (SPI / RMT / I2C ...) or a timing-critical
     * section (insomnia) needs it. This roughly halves idle CPU draw, which is
     * the dominant battery sink on this port. Nothing ever lowered the fixed
     * 160 MHz clock before.
     *
     * DFS 80-160 MHz plus automatic light sleep, the latter gated by the
     * NO_LIGHT_SLEEP lock below. min_freq is 80 (not 40) so APB stays
     * PLL-sourced and the LEDC backlight PWM and I2C timing don't shift. */
    furi_hal_power_ls_mutex = xSemaphoreCreateMutexStatic(&furi_hal_power_ls_mutex_buffer);

    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 80,
        .light_sleep_enable = true,
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
        } else {
            esp_pm_lock_acquire(furi_hal_power_no_ls_lock);
        }
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
        esp_pm_light_sleep_register_cbs(&furi_hal_power_ls_cbs);
#endif
        ESP_LOGI(TAG, "DFS 80-160 MHz + gated light sleep enabled");
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

    /* BQ27220 first (needs clean I2C bus), then BQ25896 */
    furi_hal_bq27220_init();
    furi_hal_bq25896_init();
    furi_hal_power_refresh_sample();
    if(furi_hal_bq27220_is_present()) {
        ESP_LOGI(TAG, "Fuel gauge: BQ27220 (%umV, %u%%)",
            furi_hal_bq27220_get_voltage_mv(), furi_hal_bq27220_get_charge_pct());
    }
    if(furi_hal_bq25896_is_present()) {
        ESP_LOGI(TAG, "Charger: BQ25896 (VBUS=%umV, VREG=%umV)",
            furi_hal_bq25896_get_vbus_voltage_mv(), furi_hal_bq25896_get_vreg_voltage_mv());
    }
}

FuriPubSub* furi_hal_power_get_idle_pubsub(void) {
    return furi_hal_power_idle_pubsub;
}

void furi_hal_power_publish_idle_state(bool idle) {
    if(__atomic_exchange_n(&furi_hal_power_idle, idle, __ATOMIC_ACQ_REL) == idle) return;
    if(furi_hal_power_idle_pubsub) {
        const FuriHalPowerIdleEvent event = {.idle = idle};
        furi_pubsub_publish(furi_hal_power_idle_pubsub, (void*)&event);
    }
}

bool furi_hal_power_is_idle(void) {
    return __atomic_load_n(&furi_hal_power_idle, __ATOMIC_ACQUIRE);
}

bool furi_hal_power_gauge_is_ok(void) {
    if(furi_hal_bq27220_is_present()) return true;
    if(!furi_hal_power.adc_handle) return true;
    furi_hal_power_refresh_sample();
    return furi_hal_power.last_sample_ok;
}

bool furi_hal_power_is_shutdown_requested(void) {
    const float battery_voltage = furi_hal_power_get_estimated_battery_voltage();
    return !furi_hal_power_is_usb_present() &&
           (battery_voltage > 0.0f) &&
           (battery_voltage <= FURI_HAL_POWER_LOW_BATTERY_THRESHOLD_V);
}

uint16_t furi_hal_power_insomnia_level(void) {
    return furi_hal_power.insomnia;
}

void furi_hal_power_insomnia_enter(void) {
    FURI_CRITICAL_ENTER();
    furi_check(furi_hal_power.insomnia < UINT8_MAX);
    furi_hal_power.insomnia++;
    FURI_CRITICAL_EXIT();
    /* Acquire outside the critical section (a frequency switch busy-waits for
     * PLL relock). The PM lock is itself reference counted, so every enter
     * takes one reference and every exit drops one: two cores entering and
     * leaving concurrently can never release before acquiring. */
#if CONFIG_PM_ENABLE
    if(furi_hal_power_freq_lock) {
        esp_pm_lock_acquire(furi_hal_power_freq_lock);
    }
#endif
}

void furi_hal_power_insomnia_exit(void) {
    FURI_CRITICAL_ENTER();
    furi_check(furi_hal_power.insomnia > 0);
    furi_hal_power.insomnia--;
    FURI_CRITICAL_EXIT();
#if CONFIG_PM_ENABLE
    if(furi_hal_power_freq_lock) {
        esp_pm_lock_release(furi_hal_power_freq_lock);
    }
#endif
}

bool furi_hal_power_sleep_available(void) {
    return furi_hal_power.insomnia == 0;
}

/* VBUS state cached by the power service tick: -1 unknown, 0 absent,
 * 1 present. Lets the 4 ms input loop and the notification service check USB
 * without I2C traffic. A failed charger read stays "unknown" instead of
 * "absent": the cache lives for a full tick, and "absent" would open the
 * light-sleep gate with USB attached. */
static volatile int8_t furi_hal_power_vbus_cache = -1;

void furi_hal_power_refresh_usb_state(void) {
    int8_t state = -1;
    if(furi_hal_bq25896_is_present()) {
        bool present = false;
        if(furi_hal_bq25896_try_vbus_present(&present)) state = present ? 1 : 0;
    } else if(furi_hal_power.adc_ready) {
        furi_hal_power_refresh_sample();
        if(furi_hal_power.last_sample_ok) state = furi_hal_power_is_usb_present() ? 1 : 0;
    }
    furi_hal_power_vbus_cache = state;
}

bool furi_hal_power_is_usb_connected(void) {
    return furi_hal_power_vbus_cache == 1;
}

bool furi_hal_power_is_running_on_battery(void) {
    /* Only an explicit VBUS sense is trusted. Without a charger IC we can't
     * tell USB from battery, so assume wired and never light-sleep. The cached
     * state (refreshed by the power service tick, once a second) is used on
     * purpose: the input loop asks every 4 ms while the screen is off, and a
     * charger register read per poll kept the I2C bus and the CPU busy. Unknown
     * (-1, before the first refresh or without a sensor) counts as not on
     * battery, so the light-sleep gate stays closed. */
    return furi_hal_bq25896_is_present() && furi_hal_power_vbus_cache == 0;
}

#if CONFIG_PM_ENABLE
/* Caller holds furi_hal_power_ls_mutex. */
static void furi_hal_power_ls_update_gate_locked(void) {
    const bool allow = furi_hal_power_ls_requested && furi_hal_power_ls_inhibit == 0;
    if(allow == furi_hal_power_ls_allowed) return;
    if(allow && furi_hal_power_ls_safe_mode) {
        /* Pads first, then a dark LED ring, then the gate. */
        furi_hal_power_sleep_apply_light_sleep_pins();
        furi_hal_light_set_ws2812_suspended(FuriHalLightSuspendLightSleep, true);
    }
    const esp_err_t err = allow ? esp_pm_lock_release(furi_hal_power_no_ls_lock) :
                                  esp_pm_lock_acquire(furi_hal_power_no_ls_lock);
    if(err == ESP_OK) {
        furi_hal_power_ls_allowed = allow;
    } else {
        ESP_LOGE(TAG, "Light-sleep gate update failed: %s", esp_err_to_name(err));
    }
    if(!furi_hal_power_ls_allowed) {
        /* Awake again (or the gate failed to open): LEDs back. No-op when the
         * light-sleep reason was never set. */
        furi_hal_light_set_ws2812_suspended(FuriHalLightSuspendLightSleep, false);
    }
}
#endif

void furi_hal_power_set_light_sleep_safe_mode(bool enable) {
#if CONFIG_PM_ENABLE
    if(!furi_hal_power_ls_mutex) return;
    xSemaphoreTake(furi_hal_power_ls_mutex, portMAX_DELAY);
    if(enable != furi_hal_power_ls_safe_mode) {
        furi_hal_power_ls_safe_mode = enable;
        if(enable) {
            if(furi_hal_power_ls_allowed) {
                furi_hal_power_sleep_apply_light_sleep_pins();
                furi_hal_light_set_ws2812_suspended(FuriHalLightSuspendLightSleep, true);
            }
        } else {
            furi_hal_power_sleep_reset_light_sleep_pins();
            furi_hal_light_set_ws2812_suspended(FuriHalLightSuspendLightSleep, false);
        }
        ESP_LOGI(TAG, "Auto Light Sleep %s", enable ? "enabled" : "disabled");
    }
    xSemaphoreGive(furi_hal_power_ls_mutex);
#else
    (void)enable;
#endif
}

bool furi_hal_power_is_light_sleep_safe_mode(void) {
#if CONFIG_PM_ENABLE
    return furi_hal_power_ls_safe_mode;
#else
    return false;
#endif
}

bool furi_hal_power_is_light_sleep_active(void) {
#if CONFIG_PM_ENABLE
    return furi_hal_power_ls_allowed;
#else
    return false;
#endif
}

void furi_hal_power_allow_light_sleep(bool allow) {
#if CONFIG_PM_ENABLE
    if(!furi_hal_power_no_ls_lock || !furi_hal_power_ls_mutex) return;
    /* Called every input poll (4 ms): skip the mutex when nothing changes. The
     * request is only written below, under the mutex, so this read is either
     * current or from just before a concurrent change, and that caller's own
     * request then wins like it always did (last writer). The gate itself is
     * re-evaluated by every inhibit enter/exit, so a skipped call never leaves
     * it stale. */
    if(__atomic_load_n(&furi_hal_power_ls_requested, __ATOMIC_ACQUIRE) == allow) return;
    xSemaphoreTake(furi_hal_power_ls_mutex, portMAX_DELAY);
    __atomic_store_n(&furi_hal_power_ls_requested, allow, __ATOMIC_RELEASE);
    furi_hal_power_ls_update_gate_locked();
    xSemaphoreGive(furi_hal_power_ls_mutex);
#else
    (void)allow;
#endif
}

void furi_hal_power_light_sleep_inhibit_enter(void) {
#if CONFIG_PM_ENABLE
    if(!furi_hal_power_ls_mutex) return;
    xSemaphoreTake(furi_hal_power_ls_mutex, portMAX_DELAY);
    furi_check(furi_hal_power_ls_inhibit < UINT16_MAX);
    furi_hal_power_ls_inhibit++;
    if(furi_hal_power_no_ls_lock) furi_hal_power_ls_update_gate_locked();
    xSemaphoreGive(furi_hal_power_ls_mutex);
#endif
}

void furi_hal_power_light_sleep_inhibit_exit(void) {
#if CONFIG_PM_ENABLE
    if(!furi_hal_power_ls_mutex) return;
    xSemaphoreTake(furi_hal_power_ls_mutex, portMAX_DELAY);
    /* An enter issued before furi_hal_power_init() was not counted: ignore
     * its exit instead of underflowing. */
    if(furi_hal_power_ls_inhibit == 0) {
        xSemaphoreGive(furi_hal_power_ls_mutex);
        ESP_LOGW(TAG, "Light-sleep inhibit exit without enter");
        return;
    }
    furi_hal_power_ls_inhibit--;
    if(furi_hal_power_no_ls_lock) furi_hal_power_ls_update_gate_locked();
    xSemaphoreGive(furi_hal_power_ls_mutex);
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
    /* BQ27220 fuel gauge (T-Embed CC1101) takes priority over ADC */
    if(furi_hal_bq27220_is_present()) {
        return furi_hal_bq27220_get_charge_pct();
    }
    if(!furi_hal_power.adc_handle) {
        return 100; /* No ADC, no fuel gauge → USB powered */
    }
    const float battery_voltage = furi_hal_power_get_estimated_battery_voltage();
    if(battery_voltage <= 0.0f) {
        return 0;
    }

    return furi_hal_power_voltage_to_pct(battery_voltage);
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
    if(!furi_hal_power.has_battery_reading) {
        return false;
    }
    return furi_hal_power_is_charging() && (furi_hal_power_get_pct() >= 100);
}

/* Shared teardown before either deep sleep or a real power-off: wait for the
 * wake button to be released (so we don't immediately re-wake), then blank the
 * LED ring, kill the display and the peripheral power rail. Returns false only
 * when the BQ25896 /QON button is still held (Paradoxt), in which case the
 * charger would wake straight away and the caller restarts instead. */
static bool furi_hal_power_prepare_shutdown(void) {
    /* Wait for button release to avoid immediate wakeup */
    while(gpio_get_level((gpio_num_t)BOARD_PIN_BUTTON_BOOT) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    bool qon_released = true;
#ifdef BOARD_PIN_BQ25896_QON
    /* /QON (Back) is a charger wake input: wait up to 5 s for its release. */
    for(uint32_t waited_ms = 0;; waited_ms += 10) {
        const int qon_level = gpio_get_level((gpio_num_t)BOARD_PIN_BQ25896_QON);
#if defined(BOARD_PIN_BQ25896_QON_ACTIVE_HIGH) && BOARD_PIN_BQ25896_QON_ACTIVE_HIGH
        const bool qon_held = qon_level != 0;
#else
        const bool qon_held = qon_level == 0;
#endif
        if(!qon_held) break;
        if(waited_ms >= 5000) {
            ESP_LOGE(TAG, "BQ25896 /QON still held; not shutting down");
            qon_released = false;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#endif
    vTaskDelay(pdMS_TO_TICKS(200)); /* debounce */
    if(!qon_released) return false;

    /* WS2812: final black frame, then no writer can relight the ring. Give
     * the frame and its reset latch (>280 us) time to finish before the data
     * pin is taken over and latched low by the deep-sleep pin table. */
    if(!furi_hal_light_shutdown_ws2812()) {
        ESP_LOGW(TAG, "WS2812 black frame not confirmed");
    }
    vTaskDelay(pdMS_TO_TICKS(2));

#ifdef BOARD_PIN_LCD_BL
    furi_hal_display_set_backlight(0);
#endif
    furi_hal_display_sleep();

    /* Power down peripherals (CC1101 + WS2812) */
#ifdef BOARD_PIN_PWR_EN
    gpio_set_level((gpio_num_t)BOARD_PIN_PWR_EN, 0);
#endif
    return true;
}

/* Enter ESP32 deep sleep, waking on the BOOT/encoder button. The RTC domain
 * stays powered (~µA draw); this is not a true power cut. Does not return.
 *
 * Every connected pad is first driven / pulled to a defined idle state and
 * latched (furi_hal_power_sleep.c), so nothing floats while asleep. Holding
 * digital pads requires esp_deep_sleep_start() to run on an internal-RAM
 * stack (the power-service stack lives in PSRAM), so the final call is made
 * from a dedicated task with an internal stack. */
static void furi_hal_power_enter_deep_sleep(void) {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    /* Wake on the BOOT/encoder button (active low). Its external pull-up keeps
     * it HIGH across deep sleep; the RTC pull-up below backs that up. The wake
     * pin itself is never latched. */
#if SOC_PM_SUPPORT_EXT0_WAKEUP
    if(rtc_gpio_is_valid_gpio((gpio_num_t)BOARD_PIN_BUTTON_BOOT)) {
        rtc_gpio_init((gpio_num_t)BOARD_PIN_BUTTON_BOOT);
        rtc_gpio_set_direction((gpio_num_t)BOARD_PIN_BUTTON_BOOT, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pulldown_dis((gpio_num_t)BOARD_PIN_BUTTON_BOOT);
        rtc_gpio_pullup_en((gpio_num_t)BOARD_PIN_BUTTON_BOOT);
    }
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BOARD_PIN_BUTTON_BOOT, 0);
#else
    esp_deep_sleep_enable_gpio_wakeup(BIT(BOARD_PIN_BUTTON_BOOT), ESP_GPIO_WAKEUP_GPIO_LOW);
#endif
    /* Clear a stale deep-sleep auto-hold bit left in the RTC domain by an
     * earlier firmware; the hold is re-armed below only when needed, from an
     * internal-stack task. Exists on the S3 only (global digital hold). */
#if SOC_GPIO_SUPPORT_HOLD_IO_IN_DSLP && !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_dis();
#endif

#if defined(BOARD_HAS_PARADOXT_POWER_SEQUENCE) && BOARD_HAS_PARADOXT_POWER_SEQUENCE
    /* Charger keeps charging autonomously while the MCU sleeps on USB. Must
     * run before the I2C pads are latched. */
    if(furi_hal_bq25896_is_present() && !furi_hal_bq25896_prepare_for_sleep()) {
        ESP_LOGW(TAG, "BQ25896 sleep preparation not verified");
    }
#endif

    furi_hal_power_sleep_audit_pins();
    const bool digital_hold = furi_hal_power_sleep_isolate_pins();
#if SOC_PM_SUPPORT_RTC_PERIPH_PD
    /* RTC IO pulls and latched levels stay powered while asleep. */
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
#endif
    furi_hal_power_sleep_start_deep_sleep(digital_hold);
}

/* Deep-sleep power-off (default mode). */
void furi_hal_power_shutdown(void) {
    if(!furi_hal_power_prepare_shutdown()) {
        /* Display may already be torn down: restart cleanly instead. */
        esp_restart();
    }
    furi_hal_power_enter_deep_sleep();
}

/* Real power-off: put the BQ25896 charger into ship mode (BATFET off), which
 * physically disconnects the battery -> 0 draw, wakes only via USB plug or the
 * charger's /QON button. Only possible on battery: with USB attached the
 * charger keeps SYS powered, so we fall back to deep sleep. If there is no
 * charger at all (or the write fails) we also fall back to deep sleep. */
void furi_hal_power_off(void) {
    if(!furi_hal_power_prepare_shutdown()) {
        esp_restart();
    }

    if(furi_hal_bq25896_is_present() && !furi_hal_bq25896_is_vbus_present()) {
        furi_hal_bq25896_poweroff();
        /* BATFET cut is near-instant; give it a moment. Should not return. */
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* Fallback: on USB, no battery path, or BATFET failed -> deep sleep. */
    furi_hal_power_enter_deep_sleep();
}

#if defined(BOARD_HAS_PARADOXT_POWER_SEQUENCE) && BOARD_HAS_PARADOXT_POWER_SEQUENCE
bool furi_hal_power_off_ship_mode(void) {
    bool vbus_present = false;
    if(!furi_hal_bq25896_is_present() || !furi_hal_bq25896_get_vbus_present(&vbus_present)) {
        ESP_LOGE(TAG, "Power-off aborted: BQ25896 state unknown");
        return false;
    }
    if(!vbus_present) {
        /* Battery: cut it. The charger removes SYS at once, so nothing is torn
         * down first; returns only if the cut was not confirmed. */
        return furi_hal_bq25896_ship_mode();
    }
    /* USB keeps SYS powered: deep sleep, wake on OK (BOOT). */
    ESP_LOGI(TAG, "USB present: deep sleep with BOOT wake instead of ship mode");
    if(!furi_hal_power_prepare_shutdown()) {
        esp_restart();
    }
    furi_hal_power_enter_deep_sleep();
    return true;
}
#endif

FURI_NORETURN void furi_hal_power_reset(void) {
    esp_restart();
    __builtin_unreachable();
}

bool furi_hal_power_enable_otg(void) {
    /* OTG 5 V = the BQ25896 charger's boost output on VBUS (REG03 OTG_CONFIG). Drive it only when a
     * charger is present AND no external VBUS is already feeding the pin: the boost must not fight a
     * host/charger that is already supplying VBUS (when USB is plugged, VBUS already carries 5 V from
     * the host, so an external 5 V module is powered either way). On a board without a charger this
     * stays a pure flag so the Sub-GHz radio loaders' retry loop still terminates (they poll
     * furi_hal_power_is_otg_enabled() after each call). */
    if(furi_hal_bq25896_is_present()) {
        if(furi_hal_bq25896_is_vbus_present()) {
            ESP_LOGW(TAG, "OTG requested while VBUS present; host supplies 5 V, boost left off");
        } else {
            furi_hal_bq25896_enable_otg();
        }
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
        furi_hal_bq25896_set_vreg_voltage_mv((uint16_t)(voltage * 1000.0f));
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
        return (float)furi_hal_bq25896_get_vbus_voltage_mv() / 1000.0f;
    }
    furi_hal_power_refresh_sample();
    return furi_hal_power_is_usb_present() ? furi_hal_power.last_supply_voltage : 0.0f;
}

void furi_hal_power_enable_external_3_3v(void) {
}

void furi_hal_power_disable_external_3_3v(void) {
}

void furi_hal_power_suppress_charge_enter(void) {
    FURI_CRITICAL_ENTER();
    if(furi_hal_power.suppress_charge < UINT8_MAX) {
        furi_hal_power.suppress_charge++;
    }
    FURI_CRITICAL_EXIT();
}

void furi_hal_power_suppress_charge_exit(void) {
    FURI_CRITICAL_ENTER();
    if(furi_hal_power.suppress_charge > 0) {
        furi_hal_power.suppress_charge--;
    }
    FURI_CRITICAL_EXIT();
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
    property_value_out(&property_context, NULL, 2, "format", "minor", "0");
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
        "%u",
        2,
        "supply",
        "mv",
        (unsigned int)lroundf(furi_hal_power.last_supply_voltage * 1000.0f));
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
