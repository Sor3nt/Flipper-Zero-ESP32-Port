#include "desktop/desktop_settings.h"
#include "power_i.h"

#include <furi.h>
#include <furi_hal.h>

#include <update_util/update_operation.h>
#include <notification/notification_messages.h>
#include <notification/notification.h>

#include <loader/loader.h>
#include <furi_hal_sd.h>
#include <wifi.h>
#include <btshim.h>
#include "boards/board.h"

#define TAG "Power"

#define POWER_OFF_TIMEOUT_S  (90U)
#define POWER_POLL_PERIOD_MS (1000UL)
/* Screen off but awake (e.g. on USB, where light sleep never engages, or
 * during the light-sleep delay): only charging state, VBUS (cached), fuel
 * gauge and protection are refreshed, every 5 s instead of every second. */
#define POWER_IDLE_POLL_PERIOD_MS (5000UL)
/* Charger voltages/currents/temperatures, VREG and OTG are only shown in the
 * battery info screen: refresh them every 5 s, not on every 1 s tick. */
#define POWER_FULL_INFO_PERIOD_MS (5000UL)
/* While light-sleeping only battery protection runs, from the fuel gauge alone
 * (no charger/VBUS access). The battery drains slowly in light sleep and the
 * protection still needs its consecutive samples, so once a minute is plenty. */
#define POWER_LIGHT_SLEEP_POLL_PERIOD_MS (60000UL)
/* A refused power-off (e.g. USB plugged in at that instant) is retried. */
#define POWER_CRITICAL_RETRY_MS (30000UL)
/* Fuel-gauge current above this while light-sleeping means a charger is
 * feeding the battery: USB was plugged in. Wake so USB can enumerate, without
 * ever polling the charger's VBUS registers during sleep. */
#define POWER_LIGHT_SLEEP_CHARGE_WAKE_A (0.020f)

#define POWER_VBUS_LOW_THRESHOLD   (4.0f)
#define POWER_HEALTH_LOW_THRESHOLD (70U)

static void power_set_notification_led_sleeping(bool sleeping) {
    NotificationApp* notification = furi_record_open(RECORD_NOTIFICATION);
    notification_set_led_sleeping(notification, sleeping);
    furi_record_close(RECORD_NOTIFICATION);
}

/* Only touch the notification service on an actual change, not every tick. */
static void power_set_led_sleeping(Power* power, bool sleeping) {
    if(power->light_sleep_led_off == sleeping) return;
    power->light_sleep_led_off = sleeping;
    power_set_notification_led_sleeping(sleeping);
}

static void power_draw_battery_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    Power* power = context;
    canvas_draw_icon(canvas, 0, 0, &I_Battery_26x8);

    if(power->info.gauge_is_ok) {
        char batteryPercentile[4];
        snprintf(batteryPercentile, sizeof(batteryPercentile), "%d", power->info.charge);
        if((power->displayBatteryPercentage == DISPLAY_BATTERY_PERCENT) &&
           (power->state !=
            PowerStateCharging)) { //if display battery percentage, black background white text
            canvas_set_font(canvas, FontBatteryPercent);
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 1, 1, 22, 6);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_str_aligned(canvas, 11, 4, AlignCenter, AlignCenter, batteryPercentile);
        } else if(
            (power->displayBatteryPercentage == DISPLAY_BATTERY_INVERTED_PERCENT) &&
            (power->state !=
             PowerStateCharging)) { //if display inverted percentage, white background black text
            canvas_set_font(canvas, FontBatteryPercent);
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_str_aligned(canvas, 11, 4, AlignCenter, AlignCenter, batteryPercentile);
        } else if(
            (power->displayBatteryPercentage == DISPLAY_BATTERY_RETRO_3) &&
            (power->state != PowerStateCharging)) { //Retro style segmented display, 3 parts
            if(power->info.charge > 25) {
                canvas_draw_box(canvas, 2, 2, 6, 4);
            }
            if(power->info.charge > 50) {
                canvas_draw_box(canvas, 9, 2, 6, 4);
            }
            if(power->info.charge > 75) {
                canvas_draw_box(canvas, 16, 2, 6, 4);
            }
        } else if(
            (power->displayBatteryPercentage == DISPLAY_BATTERY_RETRO_5) &&
            (power->state != PowerStateCharging)) { //Retro style segmented display, 5 parts
            if(power->info.charge > 10) {
                canvas_draw_box(canvas, 2, 2, 3, 4);
            }
            if(power->info.charge > 30) {
                canvas_draw_box(canvas, 6, 2, 3, 4);
            }
            if(power->info.charge > 50) {
                canvas_draw_box(canvas, 10, 2, 3, 4);
            }
            if(power->info.charge > 70) {
                canvas_draw_box(canvas, 14, 2, 3, 4);
            }
            if(power->info.charge > 90) {
                canvas_draw_box(canvas, 18, 2, 3, 4);
            }
        } else if(
            (power->displayBatteryPercentage == DISPLAY_BATTERY_BAR_PERCENT) &&
            (power->state != PowerStateCharging) && // Default bar display with percentage
            (power->info.voltage_battery_charge_limit >=
             4.2)) { // not looking nice with low voltage indicator
            canvas_set_font(canvas, FontBatteryPercent);

            // align charge display value with digits to draw
            uint8_t bar_charge = power->info.charge;
            if(bar_charge > 23 && bar_charge < 38) {
                bar_charge = 23;
            } else if(bar_charge >= 38 && bar_charge < 62) {
                bar_charge = 50;
            } else if(bar_charge >= 62 && bar_charge < 74) {
                bar_charge = 74;
            }

            // drawing digits
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 1, 1, (bar_charge * 22) / 100, 6);
            if(bar_charge < 38) { // both digits are black
                canvas_set_color(canvas, ColorBlack);
                canvas_draw_str_aligned(
                    canvas, 11, 4, AlignCenter, AlignCenter, batteryPercentile);
            } else if(bar_charge >= 38 && bar_charge < 74) { // first digit is white
                canvas_set_color(canvas, ColorWhite);

                // first
                char batteryPercentileFirstDigit[2];
                snprintf(
                    batteryPercentileFirstDigit,
                    sizeof(batteryPercentileFirstDigit),
                    "%c",
                    batteryPercentile[0]);
                canvas_draw_str_aligned(
                    canvas, 9, 4, AlignCenter, AlignCenter, batteryPercentileFirstDigit);

                // second
                char batteryPercentileSecondDigit[2];
                snprintf(
                    batteryPercentileSecondDigit,
                    sizeof(batteryPercentileSecondDigit),
                    "%c",
                    batteryPercentile[1]);
                canvas_set_color(canvas, ColorBlack);
                canvas_draw_str_aligned(
                    canvas, 15, 4, AlignCenter, AlignCenter, batteryPercentileSecondDigit);
            } else { // charge >= 74, both digits are white
                canvas_set_color(canvas, ColorWhite);
                canvas_draw_str_aligned(
                    canvas, 11, 4, AlignCenter, AlignCenter, batteryPercentile);
            }

        } else { //default bar display, added here to serve as fallback/default behaviour.
            canvas_draw_box(canvas, 2, 2, (power->info.charge + 4) / 5, 4);
        }

        // TODO: Verify if it displays correctly with custom battery skins !!!
        if(power->info.voltage_battery_charge_limit < 4.2) {
            // Battery charging voltage is modified, indicate with cross pattern
            canvas_invert_color(canvas);
            uint8_t battery_bar_width = (power->info.charge + 4) / 5;
            bool cross_odd = false;
            // Start 1 further in from the battery bar's x position
            for(uint8_t x = 3; x <= battery_bar_width; x++) {
                // Cross pattern is from the center of the battery bar
                // y = 2 + 1 (inset) + 1 (for every other)
                canvas_draw_dot(canvas, x, 3 + (uint8_t)cross_odd);
                cross_odd = !cross_odd;
            }
            canvas_invert_color(canvas);
        }

        if(power->state == PowerStateCharging) {
            canvas_set_bitmap_mode(canvas, 1);
            // TODO: replace -1 magic for uint8_t with re-framing
            if(power->displayBatteryPercentage == DISPLAY_BATTERY_PERCENT) {
                canvas_set_color(canvas, ColorBlack);
                canvas_draw_box(canvas, 1, 1, 22, 6);
                canvas_draw_icon(canvas, 2, -1, &I_Charging_lightning_9x10);
                canvas_set_color(canvas, ColorWhite);
                canvas_draw_icon(canvas, 2, -1, &I_Charging_lightning_mask_9x10);
                canvas_set_font(canvas, FontBatteryPercent);
                canvas_draw_str_aligned(
                    canvas, 16, 4, AlignCenter, AlignCenter, batteryPercentile);
            } else if(power->displayBatteryPercentage == DISPLAY_BATTERY_INVERTED_PERCENT) {
                canvas_set_color(canvas, ColorWhite);
                canvas_draw_box(canvas, 1, 1, 22, 6);
                canvas_draw_icon(canvas, 2, -1, &I_Charging_lightning_9x10);
                canvas_set_color(canvas, ColorBlack);
                canvas_draw_icon(canvas, 2, -1, &I_Charging_lightning_mask_9x10);
                canvas_set_font(canvas, FontBatteryPercent);
                canvas_draw_str_aligned(
                    canvas, 16, 4, AlignCenter, AlignCenter, batteryPercentile);
            } else if(power->displayBatteryPercentage == DISPLAY_BATTERY_BAR_PERCENT) {
                // clean-up default charging bar display
                canvas_set_color(canvas, ColorWhite);
                canvas_draw_box(canvas, 1, 1, 22, 6);

                // align charge display value with digits to draw
                uint8_t bar_charge = power->info.charge;

                if(bar_charge > 48 && bar_charge < 63) {
                    bar_charge = 48;
                } else if(bar_charge >= 63 && bar_charge < 84) {
                    bar_charge = 75;
                } else if(bar_charge >= 84 && bar_charge < 96) {
                    bar_charge = 96;
                }
                canvas_set_color(canvas, ColorBlack);
                canvas_draw_box(canvas, 1, 1, (bar_charge * 22) / 100, 6);

                // drawing charge icon
                canvas_draw_icon(canvas, 2, -1, &I_Charging_lightning_9x10);
                canvas_set_color(canvas, ColorWhite);
                canvas_draw_icon(canvas, 2, -1, &I_Charging_lightning_mask_9x10);

                // drawing digits
                canvas_set_font(canvas, FontBatteryPercent);
                if(bar_charge < 64) { // both digits are black
                    canvas_set_color(canvas, ColorBlack);
                    canvas_draw_str_aligned(
                        canvas, 16, 4, AlignCenter, AlignCenter, batteryPercentile);
                } else if(bar_charge >= 64 && bar_charge < 84) { // first digit is white
                    canvas_set_color(canvas, ColorWhite);

                    // first
                    char batteryPercentileFirstDigit[2];
                    snprintf(
                        batteryPercentileFirstDigit,
                        sizeof(batteryPercentileFirstDigit),
                        "%c",
                        batteryPercentile[0]);
                    canvas_draw_str_aligned(
                        canvas, 14, 4, AlignCenter, AlignCenter, batteryPercentileFirstDigit);

                    // second
                    char batteryPercentileSecondDigit[2];
                    snprintf(
                        batteryPercentileSecondDigit,
                        sizeof(batteryPercentileSecondDigit),
                        "%c",
                        batteryPercentile[1]);
                    canvas_set_color(canvas, ColorBlack);
                    canvas_draw_str_aligned(
                        canvas, 20, 4, AlignCenter, AlignCenter, batteryPercentileSecondDigit);
                } else { // charge >= 84, both digits are white
                    canvas_set_color(canvas, ColorWhite);
                    canvas_draw_str_aligned(
                        canvas, 16, 4, AlignCenter, AlignCenter, batteryPercentile);
                }
            } else {
                canvas_set_color(canvas, ColorWhite);
                canvas_draw_icon(canvas, 8, -1, &I_Charging_lightning_mask_9x10);
                canvas_set_color(canvas, ColorBlack);
                canvas_draw_icon(canvas, 8, -1, &I_Charging_lightning_9x10);
            }
            canvas_set_bitmap_mode(canvas, 0);
        }

    } else {
        canvas_draw_box(canvas, 8, 3, 8, 2);
    }
}

static ViewPort* power_battery_view_port_alloc(Power* power) {
    ViewPort* battery_view_port = view_port_alloc();
    view_port_set_width(battery_view_port, icon_get_width(&I_Battery_26x8));
    view_port_draw_callback_set(battery_view_port, power_draw_battery_callback, power);
    return battery_view_port;
}

/* full=false (screen off): skip the charger readings that are only shown on
 * screen (voltages, currents, temperatures, VREG, OTG) and keep their last
 * values; PowerInfo stays complete for API callers. */
static bool power_update_info(Power* power, bool full) {
    if(!full) {
        PowerInfo info = power->info;
        info.is_charging = furi_hal_power_is_charging();
        info.gauge_is_ok = furi_hal_power_gauge_is_ok();
        info.is_shutdown_requested = furi_hal_power_is_shutdown_requested();
        info.charge = furi_hal_power_get_pct();
        if(!furi_hal_power_is_usb_connected()) info.voltage_vbus = 0.0f;
        /* The OTG retry below must see the real state, not a stale one. */
        if(power->is_otg_requested) info.is_otg_enabled = furi_hal_power_is_otg_enabled();
        const bool need_refresh = (power->info.charge != info.charge) ||
                                  (power->info.is_charging != info.is_charging) ||
                                  (power->info.gauge_is_ok != info.gauge_is_ok);
        power->info = info;
        return need_refresh;
    }

    const PowerInfo info = {
        .is_charging = furi_hal_power_is_charging(),
        .gauge_is_ok = furi_hal_power_gauge_is_ok(),
        .is_shutdown_requested = furi_hal_power_is_shutdown_requested(),
        .is_otg_enabled = furi_hal_power_is_otg_enabled(),
        .charge = furi_hal_power_get_pct(),
        .health = furi_hal_power_get_bat_health_pct(),
        .capacity_remaining = furi_hal_power_get_battery_remaining_capacity(),
        .capacity_full = furi_hal_power_get_battery_full_capacity(),
        .current_charger = furi_hal_power_get_battery_current(FuriHalPowerICCharger),
        .current_gauge = furi_hal_power_get_battery_current(FuriHalPowerICFuelGauge),
        .voltage_battery_charge_limit = furi_hal_power_get_battery_charge_voltage_limit(),
        .voltage_charger = furi_hal_power_get_battery_voltage(FuriHalPowerICCharger),
        .voltage_gauge = furi_hal_power_get_battery_voltage(FuriHalPowerICFuelGauge),
        .voltage_vbus = furi_hal_power_get_usb_voltage(),
        .temperature_charger = furi_hal_power_get_battery_temperature(FuriHalPowerICCharger),
        .temperature_gauge = furi_hal_power_get_battery_temperature(FuriHalPowerICFuelGauge),
    };

    const bool need_refresh = (power->info.charge != info.charge) ||
                              (power->info.is_charging != info.is_charging) ||
                              (power->info.gauge_is_ok != info.gauge_is_ok);
    power->info = info;
    return need_refresh;
}

static void power_tick_callback(void* context);

/* Only re-arm the event-loop tick on an actual change. */
static void power_set_tick_period(Power* power, uint32_t period_ms) {
    if(power->tick_period == period_ms) return;
    power->tick_period = period_ms;
    furi_event_loop_tick_set(power->event_loop, period_ms, power_tick_callback, power);
}

static void power_check_charging_state(Power* power) {
    NotificationApp* notification = furi_record_open(RECORD_NOTIFICATION);

    /* is_charging was read by power_update_info() in this same tick. */
    if(power->info.is_charging) {
        if((power->info.charge == 100) || (furi_hal_power_is_charging_done())) {
            if(power->state != PowerStateCharged) {
                notification_internal_message(notification, &sequence_charged);
                power->state = PowerStateCharged;
                power->event.type = PowerEventTypeFullyCharged;
                furi_pubsub_publish(power->event_pubsub, &power->event);
            }

        } else if(power->state != PowerStateCharging) {
            notification_internal_message(notification, &sequence_charging);
            power->state = PowerStateCharging;
            power->event.type = PowerEventTypeStartCharging;
            furi_pubsub_publish(power->event_pubsub, &power->event);
        }

    } else if(power->state != PowerStateNotCharging) {
        notification_internal_message(notification, &sequence_not_charging);
        power->state = PowerStateNotCharging;
        power->event.type = PowerEventTypeStopCharging;
        furi_pubsub_publish(power->event_pubsub, &power->event);
    }

    furi_record_close(RECORD_NOTIFICATION);
}

static bool power_check_manual_poweroff(Power* power) {
    if(!power->poweroff_prompt_active) return false;

    PowerOffResponse response = power_off_get_response(power->view_power_off);
    if(response == PowerOffResponseOk) {
        power->poweroff_prompt_active = false;
        FURI_LOG_I(TAG, "Power-off confirmation accepted");
        power_off(power);
    } else if(response == PowerOffResponseCancel || response == PowerOffResponseHide) {
        power->poweroff_prompt_active = false;
        FURI_LOG_I(TAG, "Power-off confirmation cancelled");
        view_holder_set_view(power->view_holder, NULL);
        power_off_reset_response(power->view_power_off);
    }
    return true;
}

static void power_light_sleep_exit(Power* power);

/* Critical level: disconnect the battery now. No cancellable dialog: below this
 * the cell is being deep-discharged and the 3.3 V rail is close to brown-out,
 * which only ends in reset loops that drain it further. */
static bool power_check_critical_battery(Power* power) {
    if(!furi_hal_power_is_critical_shutdown_requested()) {
        power->battery_critical = false;
        return false;
    }
    const uint32_t now = furi_get_tick();
    if(power->battery_critical &&
       (now - power->battery_critical_tick) < furi_ms_to_ticks(POWER_CRITICAL_RETRY_MS)) {
        return false;
    }
    power->battery_critical = true;
    power->battery_critical_tick = now;
    FURI_LOG_E(TAG, "Battery critical: powering off to protect the cell");
    /* Leave light sleep first; if the power-off is refused (USB plugged in at
     * that moment) the device must be fully awake again, BLE included. */
    power_light_sleep_exit(power);
    power_off(power);
    return true;
}

static void power_check_low_battery(Power* power) {
    if(!power->info.gauge_is_ok) {
        return;
    }

    // Check battery charge and vbus voltage
    if((power->info.is_shutdown_requested) &&
       (power->info.voltage_vbus < POWER_VBUS_LOW_THRESHOLD) && power->show_battery_low_warning) {
        if(!power->battery_low) {
            NotificationApp* notification = furi_record_open(RECORD_NOTIFICATION);
            notification_message(notification, &sequence_low_battery);
            furi_record_close(RECORD_NOTIFICATION);
            power_off_set_manual(power->view_power_off, false);
            view_holder_send_to_front(power->view_holder);
            view_holder_set_view(power->view_holder, power_off_get_view(power->view_power_off));
        }
        /* Resync every tick, not just on the false->true transition. If a
         * manual button shutdown interrupted this dialog and set
         * manual=true, then got cancelled, battery_low was already true and
         * the block above never re-runs -> the low-battery dialog would
         * keep showing manual-shutdown text on the next warning cycle. */
        power_off_set_manual(power->view_power_off, false);
        power->battery_low = true;
    } else {
        if(power->battery_low) {
            // view_dispatcher_switch_to_view(power->view_dispatcher, VIEW_NONE);
            view_holder_set_view(power->view_holder, NULL);
            power->power_off_timeout = POWER_OFF_TIMEOUT_S;
        }
        power->battery_low = false;
    }
    // If battery low, update view and switch off power after timeout
    if(power->battery_low) {
        PowerOffResponse response = power_off_get_response(power->view_power_off);
        if(response == PowerOffResponseDefault) {
            if(power->power_off_timeout) {
                power_off_set_time_left(power->view_power_off, power->power_off_timeout--);
            } else {
                power_off(power);
            }
        } else if(response == PowerOffResponseOk) {
            power_off(power);
        } else if(response == PowerOffResponseHide) {
            view_holder_set_view(power->view_holder, NULL);
            if(power->power_off_timeout) {
                power_off_set_time_left(power->view_power_off, power->power_off_timeout--);
            } else {
                power_off(power);
            }
        } else if(response == PowerOffResponseCancel) {
            view_holder_set_view(power->view_holder, NULL);
        }
    }
}

static void power_check_battery_level_change(Power* power) {
    if(power->battery_level != power->info.charge) {
        power->battery_level = power->info.charge;
        power->event.type = PowerEventTypeBatteryLevelChanged;
        power->event.data.battery_level = power->battery_level;
        furi_pubsub_publish(power->event_pubsub, &power->event);
    }
}

void power_trigger_ui_update(Power* power) {
    DesktopSettings* settings = malloc(sizeof(DesktopSettings));
    desktop_settings_load(settings);
    power->displayBatteryPercentage = settings->displayBatteryPercentage;
    free(settings);
    view_port_enabled_set(
        power->battery_view_port,
        power->displayBatteryPercentage != DISPLAY_BATTERY_OFF);
    view_port_update(power->battery_view_port);
}

#if defined(BOARD_HAS_BQ25896_SHIP_MODE)
static void power_prepare_for_shutdown(void) {
    FURI_LOG_I(TAG, "Shutdown prepare: checking active application");
    Loader* loader = furi_record_open(RECORD_LOADER);
    if(loader) {
        if(loader_is_locked(loader)) {
            /* The ESP32 loader API does not expose FuriSignalExit. Do not
             * claim that the application stopped; the power transition below
             * remains responsible for terminating the process. */
            FURI_LOG_W(TAG, "Active application is still running during shutdown");
        } else {
            FURI_LOG_I(TAG, "No active application is running");
        }
        furi_record_close(RECORD_LOADER);
    }

    FURI_LOG_I(TAG, "Shutdown prepare: flushing storage");
    if(furi_hal_sd_is_mounted()) {
        if(!furi_hal_sd_unmount()) {
            FURI_LOG_E(TAG, "SD unmount failed before shutdown");
        } else {
            FURI_LOG_I(TAG, "SD card unmounted before shutdown");
        }
    } else {
        FURI_LOG_I(TAG, "SD card already unmounted");
    }

    /* Display and notification hardware teardown is owned by the HAL. */
}
#endif

static void power_handle_shutdown(Power* power) {
    power->shutdown_in_progress = true;
    power->is_otg_requested = false;
    furi_hal_power_disable_otg();
    FURI_LOG_I(TAG, "Shutdown latch set; power polling and OTG requests are now blocked");
    NotificationApp* notification = furi_record_open(RECORD_NOTIFICATION);
    notification_message_block(notification, &sequence_shutdown);
    furi_record_close(RECORD_NOTIFICATION);
#if defined(BOARD_HAS_BQ25896_SHIP_MODE)
    /* Always attempt the BQ25896 BATFET path on supported boards. USB power
     * still falls back to deep sleep because VBUS keeps SYS powered. */
    power_prepare_for_shutdown();
    FURI_LOG_I(TAG, "BQ25896 shutdown: requesting charger power-off path");
    if(!furi_hal_power_off()) {
        FURI_LOG_E(TAG, "BQ25896 shutdown aborted; device remains awake");
        power->shutdown_in_progress = false;
        return;
    }
#else
    if(power->settings.off_mode == PowerOffModePowerOff) {
        /* Real power-off (charger ship mode on battery, deep-sleep fallback on USB) */
        if(!furi_hal_power_off()) {
            FURI_LOG_E(TAG, "Power-off aborted; device remains awake");
            power->shutdown_in_progress = false;
            return;
        }
    } else {
        /* Default: ESP32 deep sleep */
        furi_hal_power_shutdown();
    }
#endif
    /* Neither returns normally (deep sleep / battery cut).
     * If one does, halt as fallback. */
    furi_halt("Power off failed");
}

static void power_handle_reboot(PowerBootMode mode) {
    if(mode == PowerBootModeNormal) {
        update_operation_disarm();
    } else if(mode == PowerBootModeDfu) {
        furi_hal_rtc_set_boot_mode(FuriHalRtcBootModeDfu);
    } else if(mode == PowerBootModeUpdateStart) {
        furi_hal_rtc_set_boot_mode(FuriHalRtcBootModePreUpdate);
    } else {
        furi_crash();
    }

    furi_hal_power_reset();
}
// get settings from service to settings_app by send message to power queue
void power_api_get_settings(Power* power, PowerSettings* settings) {
    furi_assert(power);
    furi_assert(settings);

    PowerMessage msg = {
        .type = PowerMessageTypeGetSettings,
        .settings = settings,
        .lock = api_lock_alloc_locked(),
    };

    furi_check(
        furi_message_queue_put(power->message_queue, &msg, FuriWaitForever) == FuriStatusOk);
    api_lock_wait_unlock_and_free(msg.lock);
}

// set settings from settings_app to service by send message to power queue
void power_api_set_settings(Power* power, const PowerSettings* settings) {
    furi_assert(power);
    furi_assert(settings);

    PowerMessage msg = {
        .type = PowerMessageTypeSetSettings,
        .csettings = settings,
        .lock = api_lock_alloc_locked(),
    };

    furi_check(
        furi_message_queue_put(power->message_queue, &msg, FuriWaitForever) == FuriStatusOk);
    api_lock_wait_unlock_and_free(msg.lock);
}

//start furi timer for autopoweroff
static void power_start_auto_poweroff_timer(Power* power) {
    if(furi_timer_is_running(power->auto_poweroff_timer)) {
        furi_timer_stop(power->auto_poweroff_timer);
    }
    furi_timer_start(
        power->auto_poweroff_timer, furi_ms_to_ticks(power->settings.auto_poweroff_delay_ms));
}

//stop furi timer for autopoweroff
static void power_stop_auto_poweroff_timer(Power* power) {
    // Stop unconditionally. Do NOT guard with furi_timer_is_running(): that maps
    // to xTimerIsTimerActive(), which returns an OBSOLETE state while a queued
    // start command is still pending in the FreeRTOS timer-service queue (see the
    // warning in timer.h). When an app is launched right after the timer was
    // armed, is_running() still read 0, so stop was skipped and the timer fired
    // mid-app (auto power-off during e.g. SubGHz Read). Stopping an already
    // stopped timer is a harmless no-op.
    furi_timer_stop(power->auto_poweroff_timer);
}

static uint32_t power_is_running_auto_poweroff_timer(Power* power) {
    return furi_timer_is_running(power->auto_poweroff_timer);
}

// start|restart poweroff timer
static void power_auto_poweroff_callback(const void* value, void* context) {
    furi_assert(value);
    furi_assert(context);
    Power* power = context;
    power_start_auto_poweroff_timer(power);
}

// callback for poweroff timer (what we do when timer end)
static void power_auto_poweroff_timer_callback(void* context) {
    furi_assert(context);
    Power* power = context;

    // Dont poweroff device if charger connected. Charging may be suppressed
    // (charge limit), so USB presence counts as "charger connected" as well.
    if(furi_hal_power_is_charging() || furi_hal_power_is_usb_connected()) {
        FURI_LOG_D(TAG, "We dont auto_power_off until battery is charging");
        power_start_auto_poweroff_timer(power);
        return;
    }

    // Dont poweroff while an application is running. Some apps (e.g. SubGHz
    // Read) sit idle waiting for signals without any key presses, so the
    // input-driven timer never gets reset and eventually fires mid-app. The
    // Loader-event based app_running flag is supposed to disarm the timer on
    // app start, but can miss it, so re-check the loader lock directly here as
    // the source of truth and simply re-arm instead of shutting down.
    Loader* loader = furi_record_open(RECORD_LOADER);
    const bool app_running = loader_is_locked(loader);
    furi_record_close(RECORD_LOADER);
    if(app_running) {
        FURI_LOG_W(
            TAG,
            "auto-poweroff fired while an app is running (flag=%d), re-arming instead",
            power->app_running);
        power_start_auto_poweroff_timer(power);
        return;
    }

    power_off(power);
}

//start|restart timer and events subscription and callbacks for input events (we restart timer when user press keys)
static void power_auto_poweroff_arm(Power* power) {
    if(power->settings.auto_poweroff_delay_ms) {
        if(power->input_events_subscription == NULL) {
            power->input_events_subscription = furi_pubsub_subscribe(
                power->input_events_pubsub, power_auto_poweroff_callback, power);
        }
        power_start_auto_poweroff_timer(power);
    }
}

// stop timer and event subscription
static void power_auto_poweroff_disarm(Power* power) {
    power_stop_auto_poweroff_timer(power);
    if(power->input_events_subscription) {
        furi_pubsub_unsubscribe(power->input_events_pubsub, power->input_events_subscription);
        power->input_events_subscription = NULL;
    }
}

//check message queue from Loader - is some app started or not (if started we dont do auto poweroff)
static void power_loader_callback(const void* message, void* context) {
    furi_assert(context);
    Power* power = context;
    const LoaderEvent* event = message;

    // disarm timer if some apps started
    if(event->type == LoaderEventTypeApplicationBeforeLoad) {
        power->app_running = true;
        power_auto_poweroff_disarm(power);
        power->light_sleep_display_off_since = 0;
        /* Revoking the grant also queues the light-sleep wake message. */
        furi_hal_power_allow_light_sleep(false);
        // arm timer if some apps was not loaded or was stoped
    } else if(event->type == LoaderEventTypeNoMoreAppsInQueue) {
        power->app_running = false;
        power_auto_poweroff_arm(power);
    }
}

// apply power settings
static void power_settings_apply(Power* power) {
    if(power->settings.deep_sleep_usb_check_interval_minutes <
           POWER_SETTINGS_INTERVAL_MIN_MINUTES ||
       power->settings.deep_sleep_usb_check_interval_minutes >
           POWER_SETTINGS_INTERVAL_MAX_MINUTES) {
        power->settings.deep_sleep_usb_check_interval_minutes =
            POWER_SETTINGS_DEFAULT_DEEP_SLEEP_USB_INTERVAL_MINUTES;
    }
    if(power->settings.light_sleep_ship_mode_interval_minutes <
           POWER_SETTINGS_INTERVAL_MIN_MINUTES ||
       power->settings.light_sleep_ship_mode_interval_minutes >
           POWER_SETTINGS_INTERVAL_MAX_MINUTES) {
        power->settings.light_sleep_ship_mode_interval_minutes =
            POWER_SETTINGS_DEFAULT_LIGHT_SLEEP_SHIP_INTERVAL_MINUTES;
    }
    if(!power->settings.light_sleep_enabled) {
        power->settings.light_sleep_ship_mode_enabled = false;
    }
#if defined(BOARD_HAS_BQ25896_SHIP_MODE) && BOARD_HAS_BQ25896_SHIP_MODE
    furi_hal_power_set_usb_deep_sleep_check(
        power->settings.deep_sleep_usb_check_enabled,
        (uint32_t)power->settings.deep_sleep_usb_check_interval_minutes * 60U);
#endif
    //apply auto_poweroff settings
    if(power->settings.auto_poweroff_delay_ms && !power->app_running) {
        power_auto_poweroff_arm(power);
    } else if(power_is_running_auto_poweroff_timer(power)) {
        power_auto_poweroff_disarm(power);
    }
    furi_hal_power_set_light_sleep_policy(
        power->settings.light_sleep_enabled, power->settings.light_sleep_delay_ms);
}

/* Queue the wake message (any thread, never blocks, at most one pending). */
static void power_light_sleep_request_wake(Power* power) {
    /* Also used to get back to the 1 s tick as soon as the user is back. */
    const bool slow = power->light_sleep_entered || power->tick_period != POWER_POLL_PERIOD_MS;
    if(!slow || power->light_sleep_wake_pending) return;
    power->light_sleep_wake_pending = true;
    PowerMessage msg = {.type = PowerMessageTypeLightSleepWake};
    if(furi_message_queue_put(power->message_queue, &msg, 0) != FuriStatusOk) {
        /* Queue full: the next (slow) tick notices the revoked grant instead. */
        power->light_sleep_wake_pending = false;
    }
}

static void power_light_sleep_revoke_callback(void* context) {
    power_light_sleep_request_wake(context);
}

#if defined(BOARD_HAS_BQ25896_SHIP_MODE) && BOARD_HAS_BQ25896_SHIP_MODE
static void power_light_sleep_ship_timeout_callback(void* context) {
    furi_assert(context);
    Power* power = context;
    power->light_sleep_ship_timeout_pending = true;
    PowerMessage msg = {.type = PowerMessageTypeLightSleepShipTimeout};
    furi_message_queue_put(power->message_queue, &msg, 0);
}
#endif

static void power_light_sleep_input_callback(const void* value, void* context) {
    UNUSED(value);
    Power* power = context;
    power->light_sleep_last_activity_tick = furi_get_tick();
    power_light_sleep_request_wake(power);
}

static void power_light_sleep_enter(Power* power) {
    /* Last VBUS read before sleeping: light sleep only ever uses this cache. */
    furi_hal_power_refresh_vbus_cache();
    if(!furi_hal_power_is_running_on_battery()) return;

    FURI_LOG_I(TAG, "Light sleep: entering");

    /* 1. Bluetooth enabled but idle (advertising only): stop the stack. The BT
     *    controller holds a NO_LIGHT_SLEEP lock while it runs. In use (connected,
     *    RPC, pairing, beacon, app profile) it is left alone and light sleep
     *    stays blocked by its inhibit. */
    Bt* bt = furi_record_open(RECORD_BT);
    power->light_sleep_bt_suspended = bt_sleep_suspend(bt);
    furi_record_close(RECORD_BT);

    /* 2. WS2812 ring: black frame now, data line held low while asleep. */
    power_set_led_sleeping(power, true);

    /* 3. Charger battery monitor off (its REGN raises quiescent current). */
    furi_hal_power_set_charger_adc_enabled(false);

    /* 4. Pause periodic timers in other services (clock, icons, animations). */
    furi_hal_power_publish_idle_state(true);

    /* 5. Grant light sleep; the HAL opens the gate once no inhibitor is held. */
    power->light_sleep_entered = true;
    power->light_sleep_wake_pending = false;
    power->light_sleep_ship_timeout_pending = false;
    power->light_sleep_ship_started_tick = furi_get_tick();
    power->light_sleep_last_activity_tick = power->light_sleep_ship_started_tick;
    furi_hal_power_allow_light_sleep(true);

    /* 6. Stop waking up every second. */
    power_set_tick_period(power, POWER_LIGHT_SLEEP_POLL_PERIOD_MS);
#if defined(BOARD_HAS_BQ25896_SHIP_MODE) && BOARD_HAS_BQ25896_SHIP_MODE
    if(power->settings.light_sleep_ship_mode_enabled) {
        const uint32_t timeout_ms =
            (uint32_t)power->settings.light_sleep_ship_mode_interval_minutes * 60000U;
        if(furi_timer_start(power->light_sleep_ship_timer, furi_ms_to_ticks(timeout_ms)) !=
           FuriStatusOk) {
            FURI_LOG_E(TAG, "Failed to arm light-sleep ship-mode timeout");
        }
    }
#endif
}

static void power_light_sleep_exit(Power* power) {
    if(!power->light_sleep_entered) return;
    power->light_sleep_entered = false;
#if defined(BOARD_HAS_BQ25896_SHIP_MODE) && BOARD_HAS_BQ25896_SHIP_MODE
    furi_timer_stop(power->light_sleep_ship_timer);
    power->light_sleep_ship_timeout_pending = false;
    power->light_sleep_ship_started_tick = 0;
    power->light_sleep_last_activity_tick = 0;
#endif

    furi_hal_power_allow_light_sleep(false);
    power_set_tick_period(power, POWER_POLL_PERIOD_MS);
    furi_hal_power_set_charger_adc_enabled(true);
    furi_hal_power_refresh_vbus_cache();
    furi_hal_power_publish_idle_state(false);
    power_set_led_sleeping(power, false);
    if(power->light_sleep_bt_suspended) {
        power->light_sleep_bt_suspended = false;
        Bt* bt = furi_record_open(RECORD_BT);
        bt_sleep_resume(bt); /* queued: the UI does not wait for the BLE bring-up */
        furi_record_close(RECORD_BT);
    }
    /* The configured delay starts again from the next screen-off. */
    power->light_sleep_display_off_since = 0;
    power->light_sleep_wake_pending = false;
    FURI_LOG_I(TAG, "Light sleep: woke up");
}

#if defined(BOARD_HAS_BQ25896_SHIP_MODE) && BOARD_HAS_BQ25896_SHIP_MODE
static bool power_light_sleep_handle_ship_timeout(Power* power) {
    if(!power->settings.light_sleep_ship_mode_enabled || !power->settings.light_sleep_enabled) {
        power->light_sleep_ship_timeout_pending = false;
        return false;
    }
    const uint32_t timeout_ms =
        (uint32_t)power->settings.light_sleep_ship_mode_interval_minutes * 60000U;
    if(!power->light_sleep_ship_timeout_pending) {
        const uint32_t elapsed_ticks = furi_get_tick() - power->light_sleep_ship_started_tick;
        if(!power->light_sleep_entered ||
           elapsed_ticks < furi_ms_to_ticks(timeout_ms)) {
            return false;
        }
    }
    power->light_sleep_ship_timeout_pending = false;

    if(!power->light_sleep_entered || power->light_sleep_wake_pending || power->app_running ||
       !power->settings.light_sleep_enabled || !furi_hal_display_is_asleep() ||
       !furi_hal_power_is_light_sleep_requested()) {
        return true;
    }
    if((furi_get_tick() - power->light_sleep_last_activity_tick) <
       furi_ms_to_ticks(timeout_ms)) {
        FURI_LOG_I(TAG, "Auto ship mode cancelled: input activity was detected");
        power_light_sleep_exit(power);
        return true;
    }

    FURI_LOG_W(
        TAG,
        "Light sleep idle for %u minutes; checking USB before BQ25896 ship mode",
        (unsigned)power->settings.light_sleep_ship_mode_interval_minutes);
    power_light_sleep_exit(power);
    if(!furi_hal_power_refresh_vbus_cache()) {
        FURI_LOG_E(TAG, "Auto ship mode cancelled: USB state could not be verified");
        return true;
    }
    if(furi_hal_power_is_usb_connected()) {
        FURI_LOG_I(TAG, "Auto ship mode cancelled: USB connected during light sleep");
        return true;
    }
    power_handle_shutdown(power);
    return true;
}
#endif

/* Close a pending grant without spinning the mutex every second. */
static void power_light_sleep_hold_awake(Power* power, bool reset_delay) {
    if(reset_delay) power->light_sleep_display_off_since = 0;
    power_set_led_sleeping(power, false);
    if(furi_hal_power_is_light_sleep_requested()) {
        furi_hal_power_allow_light_sleep(false);
    }
}

static void power_light_sleep_tick(Power* power) {
    if(power->light_sleep_entered) {
        /* Fallback for a wake message that could not be queued. */
        if(!furi_hal_display_is_asleep() || !furi_hal_power_is_light_sleep_requested()) {
            power_light_sleep_exit(power);
        }
        return;
    }

    uint32_t display_transition_count;
    const bool display_is_asleep =
        furi_hal_display_get_sleep_state(&display_transition_count);
    if(display_transition_count != power->light_sleep_display_transition_count) {
        power->light_sleep_display_transition_count = display_transition_count;
        power->light_sleep_display_off_since = 0;
    }

    if(!display_is_asleep || !furi_hal_power_is_running_on_battery()) {
        power_light_sleep_hold_awake(power, true);
        return;
    }

    Loader* loader = furi_record_open(RECORD_LOADER);
    const bool app_running = loader_is_locked(loader);
    furi_record_close(RECORD_LOADER);
    if(app_running || power->is_otg_requested) {
        power_light_sleep_hold_awake(power, true);
        return;
    }

    const uint32_t now = furi_get_tick();
    if(power->light_sleep_display_off_since == 0) {
        power->light_sleep_display_off_since = now ? now : 1;
    }

    uint32_t delay_ms = 0;
    const bool enabled = furi_hal_power_get_light_sleep_policy(&delay_ms);
    if(!enabled) {
        power_light_sleep_hold_awake(power, false);
        return;
    }
    const uint32_t elapsed_ms = (uint32_t)(
        (uint64_t)(now - power->light_sleep_display_off_since) * 1000U /
        furi_kernel_get_tick_frequency());
    if(elapsed_ms < delay_ms) {
        power_light_sleep_hold_awake(power, false);
        return;
    }

    Wifi* wifi = furi_record_open(RECORD_WIFI);
    const bool wifi_connected = wifi && wifi_is_connected(wifi);
    furi_record_close(RECORD_WIFI);
    if(wifi_connected) {
        power_light_sleep_hold_awake(power, false);
        return;
    }

    power_light_sleep_enter(power);
}

// do something depend from power queue message
static void power_message_callback(FuriEventLoopObject* object, void* context) {
    furi_assert(context);
    Power* power = context;

    furi_assert(object == power->message_queue);

    PowerMessage msg;
    furi_check(furi_message_queue_get(power->message_queue, &msg, 0) == FuriStatusOk);

    switch(msg.type) {
    case PowerMessageTypeShutdown:
        if(power->shutdown_in_progress) break;
        power_handle_shutdown(power);
        break;
    case PowerMessageTypeShowPoweroffConfirmation:
        if(!power->shutdown_in_progress && !power->poweroff_prompt_active) {
            power->poweroff_prompt_active = true;
            FURI_LOG_I(TAG, "Showing power-off confirmation");
            power_off_set_manual(power->view_power_off, true);
            power_off_reset_response(power->view_power_off);
            view_holder_send_to_front(power->view_holder);
            view_holder_set_view(power->view_holder, power_off_get_view(power->view_power_off));
        }
        break;
    case PowerMessageTypeReboot:
        power_handle_reboot(msg.boot_mode);
        break;
    case PowerMessageTypeGetInfo:
        *msg.power_info = power->info;
        break;
    case PowerMessageTypeIsBatteryHealthy:
        *msg.bool_param = power->info.health > POWER_HEALTH_LOW_THRESHOLD;
        break;
    case PowerMessageTypeShowBatteryLowWarning:
        power->show_battery_low_warning = *msg.bool_param;
        break;
    case PowerMessageTypeGetSettings:
        furi_assert(msg.lock);
        *msg.settings = power->settings;
        break;
    case PowerMessageTypeSetSettings:
        furi_assert(msg.lock);
        power->settings = *msg.csettings;
        power_settings_apply(power);
        power_settings_save(&power->settings);
        break;
    case PowerMessageTypeReloadSettings:
        power_settings_load(&power->settings);
        power_settings_apply(power);
        break;
    case PowerMessageTypeSwitchOTG:
        if(power->shutdown_in_progress) {
            power->is_otg_requested = false;
            furi_hal_power_disable_otg();
            break;
        }
        power->is_otg_requested = *msg.bool_param;
        if(power->is_otg_requested) {
            power->info.voltage_vbus = furi_hal_power_get_usb_voltage();
            // Only try to enable if VBUS voltage is low, otherwise charger will refuse
            if(power->info.voltage_vbus < 4.5f) {
                bool enabled = false;
                for(size_t attempt = 0; attempt < 5; attempt++) {
                    if(furi_hal_power_enable_otg()) {
                        enabled = true;
                        break;
                    }
                }
                if(!enabled) {
                    FURI_LOG_W(TAG, "Failed to enable OTG, will try later");
                }
            } else {
                FURI_LOG_W(
                    TAG,
                    "Postponing OTG enable: VBUS(%0.1f) >= 4.5v",
                    (double)power->info.voltage_vbus);
            }
        } else {
            furi_hal_power_disable_otg();
        }
        break;
    case PowerMessageTypeLightSleepWake:
        power->light_sleep_wake_pending = false;
        if(power->light_sleep_entered) {
            power_light_sleep_exit(power);
        } else {
            power_set_tick_period(power, POWER_POLL_PERIOD_MS);
        }
        power->info_full_tick = 0; /* screen back on: full refresh right away */
        break;
        case PowerMessageTypeLightSleepShipTimeout:
    #if defined(BOARD_HAS_BQ25896_SHIP_MODE) && BOARD_HAS_BQ25896_SHIP_MODE
        power_light_sleep_handle_ship_timeout(power);
    #endif
        break;
    default:
        furi_crash();
    }

    if(msg.lock) {
        api_lock_unlock(msg.lock);
    }
}

static void power_charge_supress(Power* power) {
    // if charge_supress_percent selected (not OFF) and current charge level equal or higher than selected level
    // then we start supression if we not supress it before.
    if(power->settings.charge_supress_percent &&
       power->info.charge >= power->settings.charge_supress_percent) {
        if(!power->charge_is_supressed) {
            power->charge_is_supressed = true;
            furi_hal_power_suppress_charge_enter();
        }
        // disable supression if charge_supress_percent OFF but charge still supressed
    } else {
        if(power->charge_is_supressed) {
            power->charge_is_supressed = false;
            furi_hal_power_suppress_charge_exit();
        }
    }
}

static void power_tick_callback(void* context) {
    furi_assert(context);
    Power* power = context;

    if(power->shutdown_in_progress) return;
    if(power_check_manual_poweroff(power)) return;

    power_light_sleep_tick(power);

    if(power->light_sleep_entered) {
#if defined(BOARD_HAS_BQ25896_SHIP_MODE) && BOARD_HAS_BQ25896_SHIP_MODE
        if(power_light_sleep_handle_ship_timeout(power)) return;
#endif
        /* Light sleep: fuel gauge only, the charger is not touched. */
        if(power_check_critical_battery(power)) return;
        if(furi_hal_power_get_battery_current(FuriHalPowerICFuelGauge) >
           POWER_LIGHT_SLEEP_CHARGE_WAKE_A) {
            FURI_LOG_I(TAG, "Light sleep: battery is charging (USB plugged in), waking up");
            power_light_sleep_exit(power);
            return;
        }
        const uint8_t charge = furi_hal_power_get_pct();
        if(charge != power->info.charge) {
            power->info.charge = charge;
            power_check_battery_level_change(power);
        }
        return;
    }

    if(power_check_critical_battery(power)) return;

    // Update data from gauge and charger
    /* Screen off and nothing on screen to count down: slow, reduced polling.
     * The low-battery dialog counts seconds, so it keeps the 1 s tick. */
    const bool idle = furi_hal_display_is_asleep() && !power->battery_low &&
                      !power->poweroff_prompt_active;
    power_set_tick_period(power, idle ? POWER_IDLE_POLL_PERIOD_MS : POWER_POLL_PERIOD_MS);
    const uint32_t now = furi_get_tick();
    const bool full = !idle && (power->info_full_tick == 0 ||
                                (now - power->info_full_tick) >=
                                    furi_ms_to_ticks(POWER_FULL_INFO_PERIOD_MS));
    if(full) power->info_full_tick = now ? now : 1;
    const bool need_refresh = power_update_info(power, full);
    // Check low battery level
    power_check_low_battery(power);
    // Check and notify about charging state
    power_check_charging_state(power);
    // Check and notify about battery level change
    power_check_battery_level_change(power);
    // charge supress arm/disarm
    power_charge_supress(power);
    // Update battery view port
    if(need_refresh) {
        view_port_update(power->battery_view_port);
    }
    // Check OTG status, disable in case of a fault
    if(furi_hal_power_check_otg_fault()) {
        FURI_LOG_E(TAG, "OTG fault detected, disabling OTG");
        furi_hal_power_disable_otg();
        power->is_otg_requested = false;
    }

    // Change OTG state if needed (i.e. after disconnecting USB power)
    if(power->is_otg_requested &&
       (!power->info.is_otg_enabled && power->info.voltage_vbus < 4.5f)) {
        FURI_LOG_D(TAG, "OTG requested but not enabled, enabling OTG");
        furi_hal_power_enable_otg();
    }
}

static void power_storage_callback(const void* message, void* context) {
    furi_assert(context);
    Power* power = context;
    const StorageEvent* event = message;

    if(event->type == StorageEventTypeCardMount) {
        PowerMessage msg = {
            .type = PowerMessageTypeReloadSettings,
        };

        furi_check(
            furi_message_queue_put(power->message_queue, &msg, FuriWaitForever) == FuriStatusOk);
    }
}

// loading and initializing power service settings
static void power_init_settings(Power* power) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    furi_pubsub_subscribe(storage_get_pubsub(storage), power_storage_callback, power);

    power_settings_load(&power->settings);
    power_settings_apply(power);
    furi_record_close(RECORD_STORAGE);
    power->charge_is_supressed = false;
}

static Power* power_alloc(void) {
    Power* power = calloc(1, sizeof(Power));
    // Pubsub
    power->event_pubsub = furi_pubsub_alloc();
    // State initialization
    power->power_off_timeout = POWER_OFF_TIMEOUT_S;
    power->show_battery_low_warning = true;

    // Load UI settings
    DesktopSettings* settings = malloc(sizeof(DesktopSettings));
    desktop_settings_load(settings);
    power->displayBatteryPercentage = settings->displayBatteryPercentage;
    free(settings);

    // auto_poweroff
    //---define subscription to loader events message (info about started apps) and define callback for this
    Loader* loader = furi_record_open(RECORD_LOADER);
    furi_pubsub_subscribe(loader_get_pubsub(loader), power_loader_callback, power);
    power->input_events_pubsub = furi_record_open(RECORD_INPUT_EVENTS);
    //define autopoweroff timer and they callback
    power->auto_poweroff_timer =
        furi_timer_alloc(power_auto_poweroff_timer_callback, FuriTimerTypeOnce, power);
#if defined(BOARD_HAS_BQ25896_SHIP_MODE) && BOARD_HAS_BQ25896_SHIP_MODE
    power->light_sleep_ship_timer =
        furi_timer_alloc(power_light_sleep_ship_timeout_callback, FuriTimerTypeOnce, power);
#endif

    // Gui
    Gui* gui = furi_record_open(RECORD_GUI);

    power->view_holder = view_holder_alloc();
    power->view_power_off = power_off_alloc();
    power->view_power_unplug_usb = power_unplug_usb_alloc();

    view_holder_attach_to_gui(power->view_holder, gui);
    // Battery view port
    power->battery_view_port = power_battery_view_port_alloc(power);
    view_port_enabled_set(
        power->battery_view_port,
        power->displayBatteryPercentage != DISPLAY_BATTERY_OFF);
    gui_add_view_port(gui, power->battery_view_port, GuiLayerStatusBarRight);
    // Event loop
    power->event_loop = furi_event_loop_alloc();
    /* 8: room for the light-sleep wake message next to API requests. */
    power->message_queue = furi_message_queue_alloc(8, sizeof(PowerMessage));

    /* Leave light sleep on any input event and whenever the grant is revoked
     * (display woken, app starting), independent of the tick period. */
    power->light_sleep_input_subscription = furi_pubsub_subscribe(
        power->input_events_pubsub, power_light_sleep_input_callback, power);
    furi_hal_power_set_light_sleep_revoke_callback(power_light_sleep_revoke_callback, power);

    furi_event_loop_subscribe_message_queue(
        power->event_loop,
        power->message_queue,
        FuriEventLoopEventIn,
        power_message_callback,
        power);
    power->tick_period = POWER_POLL_PERIOD_MS;
    furi_event_loop_tick_set(power->event_loop, POWER_POLL_PERIOD_MS, power_tick_callback, power);

    return power;
}

int32_t power_srv(void* p) {
    UNUSED(p);

    if(furi_hal_rtc_get_boot_mode() != FuriHalRtcBootModeNormal) {
        FURI_LOG_W(TAG, "Skipping start in special boot mode");

        furi_thread_suspend(furi_thread_get_current_id());
        return 0;
    }

    Power* power = power_alloc();

    // power service settings initialization
    power_init_settings(power);

    power_update_info(power, true);

    furi_record_create(RECORD_POWER, power);

    // Can't be done in alloc, other things in startup need power service and it would deadlock by waiting for loader
    Loader* loader = furi_record_open(RECORD_LOADER);
    power->app_running = loader_is_locked(loader);
    furi_record_close(RECORD_LOADER);

    furi_event_loop_run(power->event_loop);

    return 0;
}
