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
    furi_record_create(RECORD_INPUT_EVENTS, event_pubsub);

    target_input_init();

    FURI_LOG_I(TAG, "Input service started");

    uint32_t sequence_counter = 0;
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    uint32_t last_telemetry = furi_get_tick();
#endif

    while(true) {
        /* Permit light sleep only while the screen is off and on battery;
         * re-checked each poll so plugging USB closes the gate promptly. */
        furi_hal_power_allow_light_sleep(
            furi_hal_display_is_asleep() && furi_hal_power_is_running_on_battery());
        furi_delay_ms(INPUT_POLL_MS);
        target_input_poll(event_pubsub, &sequence_counter);

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
        /* Debug telemetry: log light-sleep engagement every 5s. Counters persist
         * while powered, so idling on battery then replugging USB reports the
         * sleep accumulated during the battery window. */
        const uint32_t now = furi_get_tick();
        if(now - last_telemetry >= furi_ms_to_ticks(5000U)) {
            last_telemetry = now;
            FuriHalPowerLightSleepStats st;
            furi_hal_power_get_light_sleep_stats(&st);
            const uint32_t up_ms = now * 1000U / furi_kernel_get_tick_frequency();
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
