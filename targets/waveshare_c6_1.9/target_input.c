/**
 * @file target_input.c
 * Input driver for Waveshare ESP32-C6-LCD-1.9: CST816S touch + BOOT button
 *
 * Gesture mapping:
 *   SlideUp    -> InputKeyLeft
 *   SlideDown  -> InputKeyRight
 *   SlideLeft  -> InputKeyDown
 *   SlideRight -> InputKeyUp
 *
 * BOOT button mapping:
 *   Single click -> InputKeyOk
 *   Double click -> InputKeyBack
 */

#include "target_input.h"

#include <furi_hal_touch.h>
#include <furi_hal_resources.h>
#include <driver/gpio.h>
#include <esp_err.h>

#define TAG "InputTouch"

/* Polling/timing constants */
#define INPUT_BUTTON_DEBOUNCE_POLLS       2U
#define INPUT_BUTTON_SHORT_PRESS_MAX_MS   300U
#define INPUT_BUTTON_DOUBLE_CLICK_MS      250U

typedef struct {
    bool raw_pressed;
    bool debounced_pressed;
    uint8_t debounce_polls;
    uint32_t press_started_at;
    bool pending_single_click;
    bool second_click_started;
    uint32_t first_click_released_at;
} InputBootButtonState;

static InputBootButtonState boot_button;
static bool touch_interaction_active;
static bool touch_gesture_sent;

/* --- helpers --- */

static void input_publish(FuriPubSub* pubsub, InputKey key, InputType type, uint32_t sequence) {
    InputEvent event = {
        .sequence_source = INPUT_SEQUENCE_SOURCE_HARDWARE,
        .sequence_counter = sequence,
        .key = key,
        .type = type,
    };
    furi_pubsub_publish(pubsub, &event);
}

static void input_emit_short(FuriPubSub* pubsub, InputKey key, uint32_t sequence) {
    input_publish(pubsub, key, InputTypePress, sequence);
    input_publish(pubsub, key, InputTypeShort, sequence);
    input_publish(pubsub, key, InputTypeRelease, sequence);
}

static bool input_map_gesture(TouchGesture gesture, InputKey* key) {
    switch(gesture) {
    case TouchGestureSlideUp:
        *key = InputKeyLeft;
        return true;
    case TouchGestureSlideDown:
        *key = InputKeyRight;
        return true;
    case TouchGestureSlideLeft:
        *key = InputKeyDown;
        return true;
    case TouchGestureSlideRight:
        *key = InputKeyUp;
        return true;
    default:
        return false;
    }
}

static uint32_t input_elapsed_ticks(uint32_t started_at, uint32_t now) {
    return now - started_at;
}

static bool input_boot_button_is_pressed(void) {
    return gpio_get_level((gpio_num_t)gpio_button_boot.pin) == 0;
}

static void input_boot_button_init(void) {
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << gpio_button_boot.pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "BOOT button gpio_config failed: %s", esp_err_to_name(err));
    }
}

static void input_boot_button_init_state(InputBootButtonState* state) {
    state->raw_pressed = input_boot_button_is_pressed();
    state->debounced_pressed = state->raw_pressed;
    state->debounce_polls = INPUT_BUTTON_DEBOUNCE_POLLS;
    state->press_started_at = 0;
    state->pending_single_click = false;
    state->second_click_started = false;
    state->first_click_released_at = 0;
}

static void input_boot_button_reset_clicks(InputBootButtonState* state) {
    state->pending_single_click = false;
    state->second_click_started = false;
    state->first_click_released_at = 0;
}

static void input_boot_button_handle_press(InputBootButtonState* state, uint32_t now) {
    state->press_started_at = now;
    if(state->pending_single_click) {
        state->second_click_started = true;
    }
}

static void input_boot_button_emit(
    FuriPubSub* pubsub,
    InputKey key,
    uint32_t* sequence_counter) {
    input_emit_short(pubsub, key, ++(*sequence_counter));
}

static void input_boot_button_handle_release(
    InputBootButtonState* state,
    FuriPubSub* pubsub,
    uint32_t now,
    uint32_t short_press_max_ticks,
    uint32_t double_click_ticks,
    uint32_t* sequence_counter) {
    bool is_short_press = input_elapsed_ticks(state->press_started_at, now) <= short_press_max_ticks;

    if(!is_short_press) {
        if(state->pending_single_click && state->second_click_started) {
            input_boot_button_emit(pubsub, InputKeyOk, sequence_counter);
        }
        input_boot_button_reset_clicks(state);
        return;
    }

    if(!state->pending_single_click) {
        state->pending_single_click = true;
        state->second_click_started = false;
        state->first_click_released_at = now;
        return;
    }

    if(input_elapsed_ticks(state->first_click_released_at, now) <= double_click_ticks) {
        input_boot_button_emit(pubsub, InputKeyBack, sequence_counter);
        input_boot_button_reset_clicks(state);
        return;
    }

    input_boot_button_emit(pubsub, InputKeyOk, sequence_counter);
    state->pending_single_click = true;
    state->second_click_started = false;
    state->first_click_released_at = now;
}

static void input_boot_button_poll(
    InputBootButtonState* state,
    FuriPubSub* pubsub,
    uint32_t now,
    uint32_t short_press_max_ticks,
    uint32_t double_click_ticks,
    uint32_t* sequence_counter) {
    bool raw_pressed = input_boot_button_is_pressed();

    if(raw_pressed == state->raw_pressed) {
        if(state->debounce_polls < INPUT_BUTTON_DEBOUNCE_POLLS) {
            state->debounce_polls++;
        }
    } else {
        state->raw_pressed = raw_pressed;
        state->debounce_polls = 1;
    }

    if((state->debounce_polls >= INPUT_BUTTON_DEBOUNCE_POLLS) &&
       (state->debounced_pressed != state->raw_pressed)) {
        state->debounced_pressed = state->raw_pressed;
        if(state->debounced_pressed) {
            input_boot_button_handle_press(state, now);
        } else {
            input_boot_button_handle_release(
                state,
                pubsub,
                now,
                short_press_max_ticks,
                double_click_ticks,
                sequence_counter);
        }
    }

    if(state->pending_single_click && !state->second_click_started &&
       (input_elapsed_ticks(state->first_click_released_at, now) > double_click_ticks)) {
        input_boot_button_emit(pubsub, InputKeyOk, sequence_counter);
        input_boot_button_reset_clicks(state);
    }
}

/* --- target_input interface --- */

void target_input_init(void) {
    furi_hal_touch_init();
    input_boot_button_init();
    input_boot_button_init_state(&boot_button);
    touch_interaction_active = false;
    touch_gesture_sent = false;
    FURI_LOG_I(TAG, "Touch + BOOT button input initialized");
}

void target_input_poll(FuriPubSub* pubsub, uint32_t* sequence_counter) {
    uint32_t now = furi_get_tick();
    uint32_t short_press_max_ticks = furi_ms_to_ticks(INPUT_BUTTON_SHORT_PRESS_MAX_MS);
    uint32_t double_click_ticks = furi_ms_to_ticks(INPUT_BUTTON_DOUBLE_CLICK_MS);

    input_boot_button_poll(
        &boot_button,
        pubsub,
        now,
        short_press_max_ticks,
        double_click_ticks,
        sequence_counter);

    TouchData touch;
    if(!furi_hal_touch_read(&touch)) {
        return;
    }

    bool touching = (touch.finger_count > 0);

    if(touching && !touch_interaction_active) {
        touch_interaction_active = true;
        touch_gesture_sent = false;
    }

    if(touch_interaction_active && touch.gesture != 0 && !touch_gesture_sent) {
        InputKey key;
        if(input_map_gesture(touch.gesture, &key)) {
            input_emit_short(pubsub, key, ++(*sequence_counter));
            touch_gesture_sent = true;
        }
    }

    if(touch_interaction_active && !touching) {
        touch_interaction_active = false;
        touch_gesture_sent = false;
    }
}
