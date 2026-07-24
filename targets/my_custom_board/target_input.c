/**
 * @file target_input.c
 * Input driver for the custom board using dedicated button pins.
 */

#include "target_input.h"

#include <furi_hal_resources.h>
#include <boards/board.h>
#include <driver/gpio.h>
#include <esp_err.h>

#define TAG "InputCustom"

#define INPUT_DEBOUNCE_POLLS 2U

typedef struct {
    gpio_num_t gpio;
    bool inverted;
    bool raw_pressed;
    bool debounced_pressed;
    uint8_t debounce_polls;
} ButtonState;

static ButtonState btn_up;
static ButtonState btn_down;
static ButtonState btn_left;
static ButtonState btn_right;
static ButtonState btn_ok;
static ButtonState btn_back;

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

static bool button_is_pressed(ButtonState* btn) {
    int level = gpio_get_level(btn->gpio);
    return btn->inverted ? (level == 0) : (level != 0);
}

static void button_init_gpio(gpio_num_t pin, bool pull_up) {
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull_up ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "GPIO %d config failed: %s", pin, esp_err_to_name(err));
    }
}

static void button_init_state(ButtonState* btn, gpio_num_t gpio, bool inverted) {
    btn->gpio = gpio;
    btn->inverted = inverted;
    btn->raw_pressed = button_is_pressed(btn);
    btn->debounced_pressed = btn->raw_pressed;
    btn->debounce_polls = INPUT_DEBOUNCE_POLLS;
}

static void button_poll(ButtonState* btn, FuriPubSub* pubsub, InputKey key, uint32_t* sequence_counter) {
    bool raw = button_is_pressed(btn);

    if(raw == btn->raw_pressed) {
        if(btn->debounce_polls < INPUT_DEBOUNCE_POLLS) {
            btn->debounce_polls++;
        }
    } else {
        btn->raw_pressed = raw;
        btn->debounce_polls = 1;
    }

    if(btn->debounce_polls < INPUT_DEBOUNCE_POLLS) return;
    if(btn->debounced_pressed == btn->raw_pressed) return;

    btn->debounced_pressed = btn->raw_pressed;

    if(btn->debounced_pressed) {
        return;
    }

    input_emit_short(pubsub, key, ++(*sequence_counter));
}

void target_input_init(void) {
    button_init_gpio((gpio_num_t)BOARD_PIN_BTN_UP, true);
    button_init_state(&btn_up, (gpio_num_t)BOARD_PIN_BTN_UP, true);

    button_init_gpio((gpio_num_t)BOARD_PIN_BTN_DOWN, true);
    button_init_state(&btn_down, (gpio_num_t)BOARD_PIN_BTN_DOWN, true);

    button_init_gpio((gpio_num_t)BOARD_PIN_BTN_LEFT, true);
    button_init_state(&btn_left, (gpio_num_t)BOARD_PIN_BTN_LEFT, true);

    button_init_gpio((gpio_num_t)BOARD_PIN_BTN_RIGHT, true);
    button_init_state(&btn_right, (gpio_num_t)BOARD_PIN_BTN_RIGHT, true);

    button_init_gpio((gpio_num_t)BOARD_PIN_BUTTON_BOOT, true);
    button_init_state(&btn_ok, (gpio_num_t)BOARD_PIN_BUTTON_BOOT, true);

    button_init_gpio((gpio_num_t)BOARD_PIN_BUTTON_KEY, true);
    button_init_state(&btn_back, (gpio_num_t)BOARD_PIN_BUTTON_KEY, true);

    FURI_LOG_I(TAG, "Custom board input initialized");
}

void target_input_poll(FuriPubSub* pubsub, uint32_t* sequence_counter) {
    button_poll(&btn_up, pubsub, InputKeyUp, sequence_counter);
    button_poll(&btn_down, pubsub, InputKeyDown, sequence_counter);
    button_poll(&btn_left, pubsub, InputKeyLeft, sequence_counter);
    button_poll(&btn_right, pubsub, InputKeyRight, sequence_counter);
    button_poll(&btn_ok, pubsub, InputKeyOk, sequence_counter);
    button_poll(&btn_back, pubsub, InputKeyBack, sequence_counter);
}
