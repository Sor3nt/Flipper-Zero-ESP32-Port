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
#define INPUT_IDLE_POLL_MS 20U

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

/* Targets without interrupt-capable inputs keep polling. */
__attribute__((weak)) bool target_input_idle_wait(uint32_t timeout_ms) {
    UNUSED(timeout_ms);
    return false;
}

/* Wake-click filter. Drivers publish into a private pubsub; this filter
 * forwards to the public RECORD_INPUT_EVENTS one. A click whose press arrives
 * while the display is off is only meant to wake the screen, so all its events
 * are re-tagged INPUT_SEQUENCE_SOURCE_HARDWARE_WAKE (see input.h).
 *
 * The display state is sampled once before each driver poll, i.e. before any
 * subscriber (notification) can wake the panel in reaction to the press, so the
 * decision cannot race with the wake-up itself. Paradoxt emits its logical
 * Short immediately after the physical Release in the same poll, so keep the
 * swallow state until that poll is over. */
typedef struct {
    FuriPubSub* output;
    bool poll_display_asleep;
    bool swallow[InputKeyMAX];
    bool release_seen[InputKeyMAX];
} InputWakeFilter;

static void input_wake_filter_callback(const void* value, void* context) {
    InputWakeFilter* filter = context;
    InputEvent event = *(const InputEvent*)value;

    if(event.sequence_source == INPUT_SEQUENCE_SOURCE_HARDWARE && event.key < InputKeyMAX) {
        if(event.type == InputTypePress && !filter->swallow[event.key] &&
           filter->poll_display_asleep) {
            filter->swallow[event.key] = true;
            filter->release_seen[event.key] = false;
        }
        if(filter->swallow[event.key]) {
            event.sequence_source = INPUT_SEQUENCE_SOURCE_HARDWARE_WAKE;
            if(event.type == InputTypeRelease) filter->release_seen[event.key] = true;
        }
    }

    furi_pubsub_publish(filter->output, &event);
}

static void input_wake_filter_poll_done(InputWakeFilter* filter) {
    for(size_t i = 0; i < InputKeyMAX; i++) {
        if(filter->release_seen[i]) {
            filter->swallow[i] = false;
            filter->release_seen[i] = false;
        }
    }
}

int32_t input_srv(void* p) {
    UNUSED(p);

    FuriPubSub* event_pubsub = furi_pubsub_alloc();
    furi_record_create(RECORD_INPUT_EVENTS, event_pubsub);

    FuriPubSub* driver_pubsub = furi_pubsub_alloc();
    InputWakeFilter wake_filter = {.output = event_pubsub};
    furi_pubsub_subscribe(driver_pubsub, input_wake_filter_callback, &wake_filter);

    target_input_init();

    FURI_LOG_I(TAG, "Input service started");

    uint32_t sequence_counter = 0;
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    uint32_t last_telemetry = furi_get_tick();
#endif

    while(true) {
        const bool display_asleep = furi_hal_display_is_asleep();
        /* No charger I2C access here: in light sleep VBUS is never polled (the
         * power service caches it before granting sleep), and while awake the
         * power service refreshes it once per second. */
        if(display_asleep && furi_hal_power_is_light_sleep_active()) {
            /* Light sleep granted: sleep until a button pin wakes the SoC
             * instead of waking it every INPUT_IDLE_POLL_MS. */
            if(!target_input_idle_wait(FuriWaitForever)) {
                furi_delay_ms(INPUT_IDLE_POLL_MS);
            }
        } else {
            furi_delay_ms(display_asleep ? INPUT_IDLE_POLL_MS : INPUT_POLL_MS);
        }
        wake_filter.poll_display_asleep = furi_hal_display_is_asleep();
        target_input_poll(driver_pubsub, &sequence_counter);
        input_wake_filter_poll_done(&wake_filter);

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
