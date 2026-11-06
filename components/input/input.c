/**
 * @file input.c
 * Input service — delegates to target-specific input driver.
 *
 * Each target provides its own target_input.c with hardware-specific
 * input handling (touch, encoder, buttons, etc.).
 */

#include "input.h"
#include "target_input.h"

#include <furi.h>
#include <furi_hal_power.h>
#include <furi_hal_display.h>

#define TAG "Input"
#define INPUT_POLL_MS 4U
/* Auto Light Sleep: slower poll while the screen is off, and how long one
 * GPIO-wake wait may block before the light-sleep gate is re-evaluated (e.g.
 * USB plugged in). */
#define INPUT_IDLE_POLL_MS 20U
#define INPUT_IDLE_WAIT_MS 1000U

typedef struct {
    FuriPubSub* event_pubsub;
    FuriPubSub* wake_event_pubsub;
    bool key_press_active[InputKeyMAX];
    bool key_press_wake_only[InputKeyMAX];
} InputEventRouter;

static void input_event_router_callback(const void* value, void* context) {
    furi_assert(value);
    furi_assert(context);

    const InputEvent* input_event = value;
    InputEventRouter* router = context;
    const bool display_asleep = furi_hal_display_is_asleep();
    bool wake_only = display_asleep;

    /* Keep a button press that began while the panel was asleep in the wake
     * stream through its release. Rotary and touch Short events have no
     * matching Press, so their asleep state is captured at the event itself. */
    const size_t key_index = (size_t)input_event->key;
    if(key_index < (size_t)InputKeyMAX) {
        const InputKey key = input_event->key;
        switch(input_event->type) {
        case InputTypePress:
            router->key_press_active[key] = true;
            router->key_press_wake_only[key] = display_asleep;
            break;
        case InputTypeShort:
        case InputTypeLong:
        case InputTypeRepeat:
        case InputTypeRelease:
            if(router->key_press_active[key]) {
                wake_only = router->key_press_wake_only[key];
            }
            if(input_event->type == InputTypeRelease) {
                router->key_press_active[key] = false;
                router->key_press_wake_only[key] = false;
            }
            break;
        default:
            break;
        }
    }

    furi_pubsub_publish(
        wake_only ? router->wake_event_pubsub : router->event_pubsub, input_event);
}

__attribute__((weak)) bool target_input_idle_wait(uint32_t timeout_ms) {
    UNUSED(timeout_ms);
    return false;
}

const char* input_get_key_name(InputKey key) {
    switch(key) {
    case InputKeyUp:
        return "Up";
    case InputKeyDown:
        return "Down";
    case InputKeyRight:
        return "Right";
    case InputKeyLeft:
        return "Left";
    case InputKeyOk:
        return "Ok";
    case InputKeyBack:
        return "Back";
    default:
        return "Unknown";
    }
}

const char* input_get_type_name(InputType type) {
    switch(type) {
    case InputTypePress:
        return "Press";
    case InputTypeRelease:
        return "Release";
    case InputTypeShort:
        return "Short";
    case InputTypeLong:
        return "Long";
    case InputTypeRepeat:
        return "Repeat";
    default:
        return "Unknown";
    }
}

int32_t input_srv(void* p) {
    UNUSED(p);

    FuriPubSub* event_pubsub = furi_pubsub_alloc();
    FuriPubSub* wake_event_pubsub = furi_pubsub_alloc();
    FuriPubSub* target_event_pubsub = furi_pubsub_alloc();
    InputEventRouter event_router = {
        .event_pubsub = event_pubsub,
        .wake_event_pubsub = wake_event_pubsub,
    };

    furi_record_create(RECORD_INPUT_EVENTS, event_pubsub);
    furi_record_create(RECORD_INPUT_WAKE_EVENTS, wake_event_pubsub);
    furi_pubsub_subscribe(target_event_pubsub, input_event_router_callback, &event_router);

    target_input_init();

    FURI_LOG_I(TAG, "Input service started");

    uint32_t sequence_counter = 0;
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    uint32_t last_telemetry = furi_get_tick();
#endif

    while(true) {
        /* Permit light sleep only while the screen is off and on battery;
         * re-checked each poll (flags and the cached VBUS only: no I2C, and
         * no lock while the request is unchanged), so plugging USB closes the
         * gate within one power-service tick (1 s). */
        const bool display_asleep = furi_hal_display_is_asleep();
        furi_hal_power_allow_light_sleep(
            display_asleep && furi_hal_power_is_running_on_battery());
        if(display_asleep && furi_hal_power_is_light_sleep_safe_mode()) {
            /* Auto Light Sleep: sleep on GPIO wake sources instead of waking
             * the SoC every few milliseconds just to poll the buttons. */
            if(!(furi_hal_power_is_light_sleep_active() &&
                 target_input_idle_wait(INPUT_IDLE_WAIT_MS))) {
                furi_delay_ms(INPUT_IDLE_POLL_MS);
            }
        } else {
            furi_delay_ms(INPUT_POLL_MS);
        }
        target_input_poll(target_event_pubsub, &sequence_counter);

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
        /* Debug telemetry: log light-sleep engagement every 5s. Counters persist
         * while powered, so idling on battery then replugging USB reports the
         * sleep accumulated during the battery window. */
        const uint32_t now = furi_get_tick();
        if(now - last_telemetry >= furi_ms_to_ticks(5000U)) {
            last_telemetry = now;
            FuriHalPowerLightSleepStats st;
            furi_hal_power_get_light_sleep_stats(&st);
            const uint32_t up_ms =
                (uint32_t)((uint64_t)now * 1000ULL / furi_kernel_get_tick_frequency());
            const uint32_t slept_ms = (uint32_t)(st.total_sleep_us / 1000ULL);
            const uint32_t pct = up_ms ? (uint32_t)((uint64_t)slept_ms * 100U / up_ms) : 0U;
            FURI_LOG_I(
                TAG,
                "lightsleep: allow=%d count=%lu slept=%lums/%lums (%lu%%) wake[t=%lu g=%lu o=%lu]",
                st.allowed ? 1 : 0, (unsigned long)st.sleep_count, (unsigned long)slept_ms,
                (unsigned long)up_ms, (unsigned long)pct, (unsigned long)st.wake_timer,
                (unsigned long)st.wake_gpio, (unsigned long)st.wake_other);
        }
#endif
    }

    return 0;
}
