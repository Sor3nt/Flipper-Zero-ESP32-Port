/**
 * @file target_input.c
 * Input driver for Waveshare ESP32-C6-Touch-LCD-1.47: AXS5106L touch + BOOT button
 *
 * The AXS5106L reports touch coordinates only (no hardware gestures like the
 * CST816S on the 1.9), so swipes are derived here from the X/Y travel between
 * touch-down and touch-up.
 *
 * Swipe mapping (calibrated empirically — see input_detect_swipe):
 *   vertical swipe   -> InputKeyUp / InputKeyDown
 *   horizontal swipe -> InputKeyLeft / InputKeyRight
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

/* Minimum travel (in touch units) to count as a swipe rather than a tap. */
#define INPUT_SWIPE_MIN_DELTA             25

/* Set to 1 to printf raw touch coordinates / detected swipes (calibration). */
#define INPUT_TOUCH_DEBUG 0

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
static bool touch_swipe_sent;
static uint16_t touch_start_x;
static uint16_t touch_start_y;
static uint16_t touch_last_x;
static uint16_t touch_last_y;

/* --- helpers --- */

static void input_publish(FuriPubSub* pubsub, InputKey key, InputType type, uint32_t sequence) {
    InputEvent event = {
        .sequence_source = INPUT_SEQUENCE_SOURCE_TOUCH,
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

/* Map a swipe delta (touch-up minus touch-down) to a key. The panel is mounted
 * rotated 90°, so the AXS5106L axes are swapped relative to the screen:
 *   touch-X (0..172) runs physically VERTICAL    -> Up/Down
 *   touch-Y (0..320) runs physically HORIZONTAL  -> Left/Right
 * Signs calibrated against measured swipes:
 *   swipe up    -> dx<0 ; swipe down  -> dx>0
 *   swipe left  -> dy<0 ; swipe right -> dy>0 */
static bool input_detect_swipe(int16_t dx, int16_t dy, InputKey* key) {
    int16_t adx = dx < 0 ? (int16_t)-dx : dx;
    int16_t ady = dy < 0 ? (int16_t)-dy : dy;

    if(adx < INPUT_SWIPE_MIN_DELTA && ady < INPUT_SWIPE_MIN_DELTA) {
        return false;
    }

    if(adx >= ady) {
        *key = (dx < 0) ? InputKeyUp : InputKeyDown;
    } else {
        *key = (dy < 0) ? InputKeyLeft : InputKeyRight;
    }
    return true;
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
    touch_swipe_sent = false;
    FURI_LOG_I(TAG, "Touch (AXS5106L) + BOOT button input initialized");
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

    if(touching) {
        if(!touch_interaction_active) {
            /* touch-down: remember the start point */
            touch_interaction_active = true;
            touch_swipe_sent = false;
            touch_start_x = touch.x;
            touch_start_y = touch.y;
#if INPUT_TOUCH_DEBUG
            /* printf goes out over USB-Serial-JTAG here (FURI_LOG doesn't). */
            printf("[TOUCH] down x=%u y=%u\n", touch.x, touch.y);
#endif
        }
        touch_last_x = touch.x;
        touch_last_y = touch.y;
    } else if(touch_interaction_active) {
        /* touch-up: evaluate the swipe */
        touch_interaction_active = false;
        int16_t dx = (int16_t)touch_last_x - (int16_t)touch_start_x;
        int16_t dy = (int16_t)touch_last_y - (int16_t)touch_start_y;
        InputKey key;
        bool is_swipe = input_detect_swipe(dx, dy, &key);
#if INPUT_TOUCH_DEBUG
        printf(
            "[TOUCH] up dx=%d dy=%d -> %s\n",
            dx,
            dy,
            is_swipe ? input_get_key_name(key) : "(tap)");
#endif
        if(is_swipe && !touch_swipe_sent) {
            input_emit_short(pubsub, key, ++(*sequence_counter));
            touch_swipe_sent = true;
        }
    }
}
