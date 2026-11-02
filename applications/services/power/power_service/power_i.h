#pragma once

#include "power.h"

#include <gui/gui.h>
#include <gui/view_holder.h>

#include <toolbox/api_lock.h>
#include <assets_icons.h>

#include "views/power_off.h"
#include "views/power_unplug_usb.h"

typedef enum {
    PowerStateNotCharging,
    PowerStateCharging,
    PowerStateCharged,
} PowerState;

struct Power {
    ViewHolder* view_holder;
    FuriPubSub* event_pubsub;
    FuriEventLoop* event_loop;
    FuriMessageQueue* message_queue;

    ViewPort* battery_view_port;
    PowerOff* view_power_off;
    PowerUnplugUsb* view_power_unplug_usb;

    PowerEvent event;
    PowerState state;
    PowerInfo info;

    bool battery_low;
    bool show_battery_low_warning;
    uint8_t displayBatteryPercentage;
    bool is_otg_requested;
    bool shutdown_in_progress;
    bool poweroff_prompt_active;
    uint8_t battery_level;
    uint8_t power_off_timeout;
    PowerSettings settings;
    FuriTimer* auto_poweroff_timer;
    FuriTimer* light_sleep_ship_timer;
    bool app_running;
    FuriPubSub* input_events_pubsub;
    FuriPubSubSubscription* input_events_subscription;
    bool charge_is_supressed;
    bool battery_critical;
    uint32_t battery_critical_tick;
    uint32_t tick_period; /* current event-loop tick, see power_set_tick_period() */
    uint32_t info_full_tick; /* last full PowerInfo refresh, 0 = refresh on next tick */
    uint32_t light_sleep_ship_started_tick;
    volatile uint32_t light_sleep_last_activity_tick;
    uint32_t light_sleep_display_off_since;
    uint32_t light_sleep_display_transition_count;
    /* Light-sleep idle state: entered = BT parked, LED dark, charger ADC off,
     * idle published, gate granted and the tick slowed down. */
    volatile bool light_sleep_entered;
    volatile bool light_sleep_wake_pending;
    volatile bool light_sleep_ship_timeout_pending;
    bool light_sleep_bt_suspended;
    bool light_sleep_led_off;
    FuriPubSubSubscription* light_sleep_input_subscription;
};

typedef enum {
    PowerViewOff,
    PowerViewUnplugUsb,
} PowerView;

typedef enum {
    PowerMessageTypeShutdown,
    PowerMessageTypeShowPoweroffConfirmation,
    PowerMessageTypeReboot,
    PowerMessageTypeGetInfo,
    PowerMessageTypeIsBatteryHealthy,
    PowerMessageTypeShowBatteryLowWarning,
    PowerMessageTypeGetSettings,
    PowerMessageTypeSetSettings,
    PowerMessageTypeReloadSettings,
    PowerMessageTypeSwitchOTG,
    PowerMessageTypeLightSleepWake,
    PowerMessageTypeLightSleepShipTimeout,
} PowerMessageType;

typedef struct {
    PowerMessageType type;
    union {
        PowerBootMode boot_mode;
        PowerInfo* power_info;
        bool* bool_param;
        PowerSettings* settings;
        const PowerSettings* csettings;
    };
    FuriApiLock lock;
} PowerMessage;
