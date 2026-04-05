#include "furi_hal_power.h"
#include "furi_hal_bq27220.h"
#include "furi_hal_bq25896.h"
#include "boards/board.h"

#include <math.h>
#include <stdio.h>

#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>
#include <driver/i2c.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <furi.h>

#include "furi_hal_resources.h"

#define TAG "FuriHalPower"

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
    furi_hal_power_ensure_initialized();

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
}

void furi_hal_power_insomnia_exit(void) {
    FURI_CRITICAL_ENTER();
    furi_check(furi_hal_power.insomnia > 0);
    furi_hal_power.insomnia--;
    FURI_CRITICAL_EXIT();
}

bool furi_hal_power_sleep_available(void) {
    return furi_hal_power.insomnia == 0;
}

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

void furi_hal_power_shutdown(void) {
    /* Configure BOOT/encoder button (GPIO0) as wake-up source (active low) */
#if SOC_PM_SUPPORT_EXT0_WAKEUP
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BOARD_PIN_BUTTON_BOOT, 0);
#else
    esp_deep_sleep_enable_gpio_wakeup(BIT(BOARD_PIN_BUTTON_BOOT), ESP_GPIO_WAKEUP_GPIO_LOW);
#endif
    esp_deep_sleep_start();
}

void furi_hal_power_off(void) {
    furi_hal_power_refresh_sample();
    if(furi_hal_power_is_usb_present()) {
        return;
    }

#if SOC_PM_SUPPORT_EXT0_WAKEUP
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BOARD_PIN_BUTTON_BOOT, 0);
#else
    esp_deep_sleep_enable_gpio_wakeup(BIT(BOARD_PIN_BUTTON_BOOT), ESP_GPIO_WAKEUP_GPIO_LOW);
#endif
    esp_deep_sleep_start();
}

FURI_NORETURN void furi_hal_power_reset(void) {
    esp_restart();
    __builtin_unreachable();
}

bool furi_hal_power_enable_otg(void) {
    furi_hal_power.otg_enabled = true;
    return true;
}

void furi_hal_power_disable_otg(void) {
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
