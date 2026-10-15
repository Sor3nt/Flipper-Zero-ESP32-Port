#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <core/common_defines.h>
#include <property.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FuriHalPowerICCharger,
    FuriHalPowerICFuelGauge,
} FuriHalPowerIC;

void furi_hal_power_init(void);
bool furi_hal_power_gauge_is_ok(void);
bool furi_hal_power_is_shutdown_requested(void);

uint16_t furi_hal_power_insomnia_level(void);
void furi_hal_power_insomnia_enter(void);
void furi_hal_power_insomnia_exit(void);
bool furi_hal_power_sleep_available(void);
void furi_hal_power_sleep(void);

uint8_t furi_hal_power_get_pct(void);
uint8_t furi_hal_power_get_bat_health_pct(void);
bool furi_hal_power_is_charging(void);
bool furi_hal_power_is_charging_done(void);

void furi_hal_power_shutdown(void);
void furi_hal_power_off(void);
FURI_NORETURN void furi_hal_power_reset(void);

bool furi_hal_power_enable_otg(void);
void furi_hal_power_disable_otg(void);
bool furi_hal_power_check_otg_fault(void);
void furi_hal_power_check_otg_status(void);
bool furi_hal_power_is_otg_enabled(void);

float furi_hal_power_get_battery_charge_voltage_limit(void);
void furi_hal_power_set_battery_charge_voltage_limit(float voltage);

uint32_t furi_hal_power_get_battery_remaining_capacity(void);
uint32_t furi_hal_power_get_battery_full_capacity(void);
uint32_t furi_hal_power_get_battery_design_capacity(void);

float furi_hal_power_get_battery_voltage(FuriHalPowerIC ic);
float furi_hal_power_get_battery_current(FuriHalPowerIC ic);
float furi_hal_power_get_battery_temperature(FuriHalPowerIC ic);
float furi_hal_power_get_usb_voltage(void);

void furi_hal_power_enable_external_3_3v(void);
void furi_hal_power_disable_external_3_3v(void);
void furi_hal_power_suppress_charge_enter(void);
void furi_hal_power_suppress_charge_exit(void);

void furi_hal_power_info_get(PropertyValueCallback callback, char sep, void* context);
void furi_hal_power_debug_get(PropertyValueCallback callback, void* context);

/** True only when a charger IC confirms VBUS is absent (running on battery).
 * False on boards without one, so they never gate light sleep on it. */
bool furi_hal_power_is_running_on_battery(void);

/** Gate for automatic light sleep: true permits it, false blocks it (the
 * default). Idempotent. Even when permitted, IDF sleeps only once every other
 * PM lock is free (insomnia, peripheral, WiFi/BT), so callers just pass the
 * idle condition (screen off and on battery). No-op without CONFIG_PM_ENABLE. */
void furi_hal_power_allow_light_sleep(bool allow);

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
/** Light-sleep instrumentation, compiled in only with the debug Kconfig option
 * CONFIG_PM_LIGHT_SLEEP_CALLBACKS. Lets a consumer confirm sleep is engaging. */
typedef struct {
    uint32_t sleep_count;
    uint64_t total_sleep_us;
    uint32_t wake_timer;
    uint32_t wake_gpio;
    uint32_t wake_other;
    bool allowed;
} FuriHalPowerLightSleepStats;

void furi_hal_power_get_light_sleep_stats(FuriHalPowerLightSleepStats* out);
#endif

#ifdef __cplusplus
}
#endif
