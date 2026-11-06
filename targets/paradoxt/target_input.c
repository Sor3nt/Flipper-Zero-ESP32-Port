/**
 * @file target_input.c
 * Input driver: Paradoxt 6-button pad.
 *
 *   - Up/Down/Left/Right: active low, internal pull-up
 *   - OK: BOOT/GPIO0, active low (external strap pull-up + internal pull-up)
 *   - Back: GPIO21 through the AO3400 inverter on the BQ25896 /QON net,
 *     active HIGH, externally biased (no internal pull). Holding Back also
 *     wakes the charger from ship mode.
 *
 * Events follow the Flipper sequence: Press on the debounced edge, Long after
 * INPUT_LONG_PRESS_MS, Repeat while held, Short (if no Long) and Release on
 * release. A button already held at boot is ignored until it is released, so
 * the OK press that woke the device from deep sleep never reaches an app.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "target_input.h"
#include "input_wake.h"

#include <furi_hal_resources.h>
#include <furi_hal_reboot_combo.h>
#include <boards/board.h>
#include <driver/gpio.h>
#include <esp_err.h>

#define TAG "InputParadoxt"

#define INPUT_DEBOUNCE_POLLS 2U
/* A level change only counts once the pin has read the same for this long. Time based,
 * not poll based: the input loop polls every 4 ms (20 ms while the screen sleeps), and two
 * equal 4 ms samples are shorter than the chatter some tact switches show on release, which
 * would be reported as a second click (one press running a command twice). */
#define INPUT_DEBOUNCE_MS    20U
#define INPUT_LONG_PRESS_MS  500U
#define INPUT_REPEAT_MS      150U

typedef struct {
    gpio_num_t gpio;
    bool active_high;
    InputKey key;
    bool raw_pressed;
    bool debounced_pressed;
    bool wait_release;
    uint8_t debounce_polls;
    uint32_t raw_changed_at;
    uint32_t press_started_at;
    bool long_press_sent;
    uint32_t last_repeat_at;
} ButtonState;

#define NUM_BUTTONS 6

static ButtonState buttons[NUM_BUTTONS];

static void input_publish(FuriPubSub* pubsub, InputKey key, InputType type, uint32_t sequence) {
    InputEvent event = {
        .sequence_source = INPUT_SEQUENCE_SOURCE_HARDWARE,
        .sequence_counter = sequence,
        .key = key,
        .type = type,
    };
    furi_pubsub_publish(pubsub, &event);
}

static bool button_is_pressed(const ButtonState* btn) {
    const int level = gpio_get_level(btn->gpio);
    return btn->active_high ? (level != 0) : (level == 0);
}

static void button_init_gpio(gpio_num_t pin, bool pull_up) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "GPIO %d config failed: %s", pin, esp_err_to_name(err));
    }
}

/* Raw level stable for at least INPUT_DEBOUNCE_POLLS reads AND INPUT_DEBOUNCE_MS. */
static bool button_is_settled(const ButtonState* btn, uint32_t now) {
    return btn->debounce_polls >= INPUT_DEBOUNCE_POLLS &&
           (now - btn->raw_changed_at) >= furi_ms_to_ticks(INPUT_DEBOUNCE_MS);
}

static void button_poll(
    ButtonState* btn,
    FuriPubSub* pubsub,
    uint32_t now,
    uint32_t long_press_ticks,
    uint32_t repeat_ticks,
    bool mute_hold,
    uint32_t* sequence_counter) {
    const bool raw = button_is_pressed(btn);

    if(raw == btn->raw_pressed) {
        if(btn->debounce_polls < INPUT_DEBOUNCE_POLLS) btn->debounce_polls++;
    } else {
        /* Any bounce restarts the stability window. */
        btn->raw_pressed = raw;
        btn->debounce_polls = 1;
        btn->raw_changed_at = now;
    }

    if(!button_is_settled(btn, now)) return;

    if(btn->wait_release) {
        if(!btn->raw_pressed) {
            btn->wait_release = false;
            btn->debounced_pressed = false;
        }
        return;
    }

    if(btn->debounced_pressed == btn->raw_pressed) {
        /* Up + Down held together is the reboot combo: no Long/Repeat, so
         * menus do not scroll while it is being held. */
        if(btn->debounced_pressed && mute_hold) {
            btn->long_press_sent = true;
            btn->last_repeat_at = now;
        } else if(btn->debounced_pressed) {
            const uint32_t held = now - btn->press_started_at;
            if(!btn->long_press_sent && held >= long_press_ticks) {
                btn->long_press_sent = true;
                btn->last_repeat_at = now;
                input_publish(pubsub, btn->key, InputTypeLong, *sequence_counter);
            } else if(btn->long_press_sent && (now - btn->last_repeat_at) >= repeat_ticks) {
                btn->last_repeat_at = now;
                input_publish(pubsub, btn->key, InputTypeRepeat, *sequence_counter);
            }
        }
        return;
    }

    btn->debounced_pressed = btn->raw_pressed;

    if(btn->debounced_pressed) {
        btn->press_started_at = now;
        btn->long_press_sent = false;
        input_publish(pubsub, btn->key, InputTypePress, ++(*sequence_counter));
    } else {
        if(!btn->long_press_sent) {
            input_publish(pubsub, btn->key, InputTypeShort, *sequence_counter);
        }
        input_publish(pubsub, btn->key, InputTypeRelease, *sequence_counter);
    }
}

void target_input_init(void) {
    /* { gpio, active high, internal pull-up, key } */
    const struct {
        gpio_num_t pin;
        bool active_high;
        bool pull_up;
        InputKey key;
    } cfg[NUM_BUTTONS] = {
        {BOARD_PIN_BTN_UP, false, true, InputKeyUp},
        {BOARD_PIN_BTN_DOWN, false, true, InputKeyDown},
        {BOARD_PIN_BTN_LEFT, false, true, InputKeyLeft},
        {BOARD_PIN_BTN_RIGHT, false, true, InputKeyRight},
        {BOARD_PIN_BTN_OK, false, true, InputKeyOk},
        {BOARD_PIN_BTN_BACK, true, false, InputKeyBack},
    };

    for(size_t i = 0; i < NUM_BUTTONS; i++) {
        button_init_gpio(cfg[i].pin, cfg[i].pull_up);

        ButtonState* b = &buttons[i];
        b->gpio = cfg[i].pin;
        b->active_high = cfg[i].active_high;
        b->key = cfg[i].key;
        b->raw_pressed = button_is_pressed(b);
        b->debounced_pressed = b->raw_pressed;
        b->wait_release = b->raw_pressed;
        b->debounce_polls = INPUT_DEBOUNCE_POLLS;
        b->raw_changed_at = furi_get_tick() - furi_ms_to_ticks(INPUT_DEBOUNCE_MS);
        b->press_started_at = 0;
        b->long_press_sent = false;
        b->last_repeat_at = 0;
    }

    /* Forced reboot (Up + Down): no-op unless CONFIG_FURI_HAL_REBOOT_COMBO */
    furi_hal_reboot_combo_init();

    /* Auto Light Sleep wake sources: all six buttons. */
    gpio_num_t wake_pins[NUM_BUTTONS];
    for(size_t i = 0; i < NUM_BUTTONS; i++) wake_pins[i] = buttons[i].gpio;
    input_wake_init(wake_pins, NUM_BUTTONS);

    FURI_LOG_I(TAG, "6-button input initialized");
}

bool target_input_idle_wait(uint32_t timeout_ms) {
    /* A press, release, long press or debounce in progress needs polling. */
    const uint32_t now = furi_get_tick();
    for(size_t i = 0; i < NUM_BUTTONS; i++) {
        const ButtonState* b = &buttons[i];
        if(b->raw_pressed || b->debounced_pressed || b->wait_release ||
           !button_is_settled(b, now)) {
            return false;
        }
    }
    InputWakePin pins[NUM_BUTTONS];
    for(size_t i = 0; i < NUM_BUTTONS; i++) {
        pins[i].pin = buttons[i].gpio;
        /* Back is active high through the /QON inverter, the rest active low. */
        pins[i].level = buttons[i].active_high ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL;
    }
    return input_wake_wait(pins, NUM_BUTTONS, timeout_ms);
}

void target_input_poll(FuriPubSub* pubsub, uint32_t* sequence_counter) {
    const uint32_t now = furi_get_tick();
    const uint32_t long_press_ticks = furi_ms_to_ticks(INPUT_LONG_PRESS_MS);
    const uint32_t repeat_ticks = furi_ms_to_ticks(INPUT_REPEAT_MS);
    /* buttons[0] = Up, buttons[1] = Down */
    const bool combo = buttons[0].raw_pressed && buttons[1].raw_pressed;

    for(size_t i = 0; i < NUM_BUTTONS; i++) {
        button_poll(
            &buttons[i],
            pubsub,
            now,
            long_press_ticks,
            repeat_ticks,
            combo && i < 2,
            sequence_counter);
    }
}
