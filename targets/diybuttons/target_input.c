 * @file target_input.c
 * Input driver: 6 GPIO buttons

#include "target_input.h"

#include <furi_hal_resources.h>
#include <boards/board.h>
#include <driver/gpio.h>
#include <esp_err.h>

#define TAG "InputButtons"

/* Timing constants */   ///// if you have trouble with buttons being to slow or double presses, mess with these constants, however they work perfectly on common tactile buttons for me
#define INPUT_DEBOUNCE_POLLS   2U
#define INPUT_LONG_PRESS_MS    500U
#define INPUT_REPEAT_MS        200U

/* Button state */
typedef struct {
    gpio_num_t gpio;
    bool       inverted;           
    InputKey   short_key;
    InputKey   long_key;
    bool       raw_pressed;
    bool       debounced_pressed;
    uint8_t    debounce_polls;
    uint32_t   press_started_at;
    bool       long_press_sent;
    uint32_t   last_repeat_at;
} ButtonState;

/* --- Button table --- */
#define NUM_BUTTONS 6

static ButtonState buttons[NUM_BUTTONS];

/* --- Helpers --- */

static void input_publish(FuriPubSub* pubsub, InputKey key, InputType type, uint32_t sequence) {
    InputEvent event = {
        .sequence_source  = INPUT_SEQUENCE_SOURCE_HARDWARE,
        .sequence_counter = sequence,
        .key              = key,
        .type             = type,
    };
    furi_pubsub_publish(pubsub, &event);
}

static void input_emit_short(FuriPubSub* pubsub, InputKey key, uint32_t sequence) {
    input_publish(pubsub, key, InputTypePress,   sequence);
    input_publish(pubsub, key, InputTypeShort,   sequence);
    input_publish(pubsub, key, InputTypeRelease, sequence);
}

static bool button_is_pressed(ButtonState* btn) {
    int level = gpio_get_level(btn->gpio);
    return btn->inverted ? (level == 0) : (level != 0);
}

static void button_init_gpio(gpio_num_t pin, bool pull_up) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = pull_up ? GPIO_PULLUP_ENABLE   : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull_up ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "GPIO %d config failed: %s", pin, esp_err_to_name(err));
    }
}

/* --- Poll one button --- */

static void button_poll(
    ButtonState* btn,
    FuriPubSub*  pubsub,
    uint32_t     now,
    uint32_t     long_press_ticks,
    uint32_t     repeat_ticks,
    uint32_t*    sequence_counter)
{
    bool raw = button_is_pressed(btn);

    /* Debounce */
    if(raw == btn->raw_pressed) {
        if(btn->debounce_polls < INPUT_DEBOUNCE_POLLS) btn->debounce_polls++;
    } else {
        btn->raw_pressed   = raw;
        btn->debounce_polls = 1;
    }

    if(btn->debounce_polls < INPUT_DEBOUNCE_POLLS) return;

    if(btn->debounced_pressed == btn->raw_pressed) {
        /* Held: long press + repeat */
        if(btn->debounced_pressed) {
            uint32_t held = now - btn->press_started_at;
            if(!btn->long_press_sent && held >= long_press_ticks) {
                btn->long_press_sent = true;
                btn->last_repeat_at  = now;
                input_publish(pubsub, btn->long_key, InputTypePress, ++(*sequence_counter));
                input_publish(pubsub, btn->long_key, InputTypeLong,  *sequence_counter);
            } else if(btn->long_press_sent && (now - btn->last_repeat_at) >= repeat_ticks) {
                btn->last_repeat_at = now;
                input_publish(pubsub, btn->long_key, InputTypeRepeat, *sequence_counter);
            }
        }
        return;
    }

    /* Edge */
    btn->debounced_pressed = btn->raw_pressed;

    if(btn->debounced_pressed) {
        /* Pressed */
        btn->press_started_at = now;
        btn->long_press_sent  = false;
    } else {
        /* Released */
        if(btn->long_press_sent) {
            input_publish(pubsub, btn->long_key, InputTypeRelease, *sequence_counter);
        } else {
            input_emit_short(pubsub, btn->short_key, ++(*sequence_counter));
        }
    }
}

//////////////

void target_input_init(void) {
    /* Define all 6 buttons: { gpio, active-low, short_key, long_key } */
    const struct { gpio_num_t pin; InputKey sk; InputKey lk; } cfg[NUM_BUTTONS] = {
        { BOARD_PIN_BTN_UP,    InputKeyUp,    InputKeyUp    },
        { BOARD_PIN_BTN_DOWN,  InputKeyDown,  InputKeyDown  },
        { BOARD_PIN_BTN_LEFT,  InputKeyLeft,  InputKeyLeft  },
        { BOARD_PIN_BTN_RIGHT, InputKeyRight, InputKeyRight },
        { BOARD_PIN_BTN_OK,    InputKeyOk,    InputKeyOk    },
        { BOARD_PIN_BTN_BACK,  InputKeyBack,  InputKeyBack  },
    };

    for(int i = 0; i < NUM_BUTTONS; i++) {
        button_init_gpio(cfg[i].pin, /*pull_up=*/true);

        ButtonState* b    = &buttons[i];
        b->gpio           = cfg[i].pin;
        b->inverted       = true;   /* all buttons active-low */
        b->short_key      = cfg[i].sk;
        b->long_key       = cfg[i].lk;
        b->raw_pressed    = button_is_pressed(b);
        b->debounced_pressed = b->raw_pressed;
        b->debounce_polls = INPUT_DEBOUNCE_POLLS;
        b->press_started_at  = 0;
        b->long_press_sent   = false;
        b->last_repeat_at    = 0;
    }

    FURI_LOG_I(TAG, "6-button input initialized");
}

void target_input_poll(FuriPubSub* pubsub, uint32_t* sequence_counter) {
    uint32_t now              = furi_get_tick();
    uint32_t long_press_ticks = furi_ms_to_ticks(INPUT_LONG_PRESS_MS);
    uint32_t repeat_ticks     = furi_ms_to_ticks(INPUT_REPEAT_MS);

    for(int i = 0; i < NUM_BUTTONS; i++) {
        button_poll(&buttons[i], pubsub, now, long_press_ticks, repeat_ticks, sequence_counter);
    }
}
