#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <core/common_defines.h>
#include <core/pubsub.h>
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

/** Paradoxt power-off (BOARD_HAS_PARADOXT_POWER_SEQUENCE only).
 *  On battery: verified BQ25896 ship mode (BATFET off), does not return on
 *  success. On USB: deep sleep with BOOT/OK wake, does not return.
 *  @return false if the charger refused or could not be read; the device
 *          stays on and nothing has been torn down */
bool furi_hal_power_off_ship_mode(void);
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
 * False on boards without one, so they never gate light sleep on it. Uses the
 * VBUS state cached by furi_hal_power_refresh_usb_state() (no I2C): up to one
 * power-service tick old, and false while unknown. */
bool furi_hal_power_is_running_on_battery(void);

/** Gate for automatic light sleep: true permits it, false blocks it (the
 * default). Idempotent. Even when permitted, IDF sleeps only once every other
 * PM lock is free (insomnia, peripheral, WiFi/BT), so callers just pass the
 * idle condition (screen off and on battery). No-op without CONFIG_PM_ENABLE. */
void furi_hal_power_allow_light_sleep(bool allow);

/** Counted veto on automatic light sleep, for sessions that must not sleep
 * (WiFi bring-up, UART bridges, buzzer tones). While the count is non-zero the
 * gate stays closed whatever furi_hal_power_allow_light_sleep() requests.
 * Pair every enter with one exit. Task context only. */
void furi_hal_power_light_sleep_inhibit_enter(void);
void furi_hal_power_light_sleep_inhibit_exit(void);

/** Auto Light Sleep (Settings > Power). When enabled, every connected pad is
 *  given a defined sleep state and the WS2812 ring is dark before automatic
 *  light sleep is allowed and the input service waits on GPIO wake sources.
 *  Disabled (default): pads and the ring are left alone and the input service
 *  keeps polling every few ms. The battery check uses the cached VBUS state in
 *  both modes. */
void furi_hal_power_set_light_sleep_safe_mode(bool enable);
bool furi_hal_power_is_light_sleep_safe_mode(void);

/** Publish display-idle transitions so periodic services can stop their timers.
 * Subscribers run synchronously in the display caller's context and must not block. */
typedef struct {
    bool idle;
} FuriHalPowerIdleEvent;

FuriPubSub* furi_hal_power_get_idle_pubsub(void);
void furi_hal_power_publish_idle_state(bool idle);
bool furi_hal_power_is_idle(void);

/** True while the light-sleep gate is open (sleep allowed). */
bool furi_hal_power_is_light_sleep_active(void);

/** Refresh the cached USB (VBUS) state. Called from the power service tick;
 *  reads the charger over I2C (or the battery ADC). */
void furi_hal_power_refresh_usb_state(void);

/** Cached USB (VBUS) state, no I2C. False when unknown or on boards that
 *  cannot sense VBUS. */
bool furi_hal_power_is_usb_connected(void);

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
