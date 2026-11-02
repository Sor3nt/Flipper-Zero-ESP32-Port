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
/** Low battery (warning level, debounced, with hysteresis). The power service
 * shows the low-battery dialog and powers off after a countdown. */
bool furi_hal_power_is_shutdown_requested(void);
/** Critical battery (debounced): the cell must be disconnected now, without a
 * cancellable prompt. Also set when the fuel gauge reports SYSDWN. */
bool furi_hal_power_is_critical_shutdown_requested(void);

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
/** Request hardware power-off. Returns false if battery ship-mode was not
 * confirmed; in that case the caller must not enter deep sleep. */
bool furi_hal_power_off(void);
/** Configure the periodic USB-removal check used by deep sleep on BQ25896 boards. */
void furi_hal_power_set_usb_deep_sleep_check(bool enabled, uint32_t interval_seconds);
FURI_NORETURN void furi_hal_power_reset(void);

bool furi_hal_power_enable_otg(void);
void furi_hal_power_disable_otg(void);
bool furi_hal_power_check_otg_fault(void);
void furi_hal_power_check_otg_status(void);
/* Reports the charger OTG output state when a BQ25896 is available. */
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
 * False on boards without one, so they never gate light sleep on it.
 * Uses the cached VBUS state (refreshed at most once per second while awake,
 * never while light sleep is active). */
bool furi_hal_power_is_running_on_battery(void);
/** True only when a charger IC confirms VBUS is present (cached, see above). */
bool furi_hal_power_is_usb_connected(void);
/** Re-read VBUS from the charger into the cache (one I2C transaction).
 * Returns false if the charger is absent or the read failed. */
bool furi_hal_power_refresh_vbus_cache(void);

/** True while the light-sleep gate is open (NO_LIGHT_SLEEP lock released),
 * i.e. the SoC is allowed to enter automatic light sleep. */
bool furi_hal_power_is_light_sleep_active(void);

/** Charger battery-monitor ADC: on while awake, off during light sleep. */
void furi_hal_power_set_charger_adc_enabled(bool enabled);

/** Idle notification for services with periodic timers (clock, icons,
 * animations, polling). Published by the power service right before light
 * sleep is allowed (idle=true) and as soon as the device wakes (idle=false).
 * Subscriber callbacks run in the power service thread and must not block. */
typedef struct {
    bool idle;
} FuriHalPowerIdleEvent;

FuriPubSub* furi_hal_power_get_idle_pubsub(void);
void furi_hal_power_publish_idle_state(bool idle);
bool furi_hal_power_is_idle(void);

/** Gate for screen-off automatic light sleep: true permits it, false blocks it.
 * Idempotent. Session inhibitors and other ESP-IDF PM locks still prevent sleep.
 * No-op without CONFIG_PM_ENABLE. */
void furi_hal_power_allow_light_sleep(bool allow);
/** True while a light-sleep grant is requested (gate may still be closed by inhibitors). */
bool furi_hal_power_is_light_sleep_requested(void);
/** Called, from the revoking thread, whenever a granted light sleep is revoked
 * (display woke up, an app starts, ...). Lets the power service leave its idle
 * state immediately whatever the wake source. Must not block. */
typedef void (*FuriHalPowerLightSleepRevokeCallback)(void* context);
void furi_hal_power_set_light_sleep_revoke_callback(
    FuriHalPowerLightSleepRevokeCallback callback,
    void* context);
/** Configure the user-controlled "Auto Light Sleep" setting and its delay. */
void furi_hal_power_set_light_sleep_policy(bool enabled, uint32_t delay_ms);
/** Read the setting; false means automatic light sleep is never granted. */
bool furi_hal_power_get_light_sleep_policy(uint32_t* delay_ms);
/** Protect a time-critical session from automatic light sleep. Calls must be paired. */
void furi_hal_power_light_sleep_inhibit_enter(void);
void furi_hal_power_light_sleep_inhibit_exit(void);

/** While `hold` is true the pad keeps a weak PULL-UP during automatic light sleep instead of
 *  its table default (header pads: pull-down). For a chip-select that belongs to a module the
 *  driver has attached: left at the default the pull-down would select the module and wake a
 *  CC1101 out of power-down. Release it (hold = false) when the pad is handed back. Returns
 *  false if too many pads are held. Only the sleep state is touched. */
bool furi_hal_power_light_sleep_hold_pin_high(uint16_t pin, bool hold);

/** Lepas semua hold GPIO sisa deep sleep sebelumnya dan serahkan tiap pin ke
 * HAL tanpa glitch (level/pull di-preload sebelum hold dilepas). Sudah
 * dipanggil otomatis oleh constructor boot awal DAN oleh furi_hal_power_init();
 * idempotent, aman dipanggil ulang kapan saja. Panggil manual hanya bila
 * constructor early-release dimatikan (FURI_HAL_POWER_EARLY_RELEASE=0) dan
 * kamu perlu menjamin urutannya sebelum HAL lain diinisialisasi. */
void furi_hal_power_release_gpio_holds(void);

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
