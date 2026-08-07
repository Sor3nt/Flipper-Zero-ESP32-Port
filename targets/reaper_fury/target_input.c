/**
 * @file target_input.c
 * Input driver for the custom board using dedicated button pins.
 *
 * Features:
 *   - 6-way button pad (Up/Down/Left/Right) + Select/OK (BOOT pin)
 *   - Back button (pin 21) with long-press detection for shutdown
 *   - Back button also wired to BQ25896 QON for power-on wake
 *   - Debouncing (2 polls)
 *   - Long-press detection: hold Back for 2.5s triggers power shutdown
 */

#include "target_input.h"

#include <furi_hal_resources.h>
#include <furi_hal_power.h>
#include <boards/board.h>
#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_timer.h>

#define TAG "InputCustom"

#define INPUT_DEBOUNCE_POLLS    2U
#define LONGPRESS_TIMER_PERIOD  50000  /* 50ms polling interval */
#define LONGPRESS_THRESHOLD_US  (BOARD_LONGPRESS_SHUTDOWN_MS * 1000)  /* 2.5s threshold */

typedef struct {
    gpio_num_t gpio;
    bool inverted;
    bool raw_pressed;
    bool debounced_pressed;
    uint8_t debounce_polls;
    int64_t press_start_us;  /* Timestamp when button transitioned to pressed */
} ButtonState;

static ButtonState btn_up;
static ButtonState btn_down;
static ButtonState btn_left;
static ButtonState btn_right;
static ButtonState btn_ok;
static ButtonState btn_back;
static esp_timer_handle_t longpress_timer = NULL;

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

static void longpress_timer_callback(void* arg) {
    /* Timer fires periodically to check for long-press on back button */
    if(btn_back.debounced_pressed) {
        int64_t now = esp_timer_get_time();
        int64_t press_duration_us = now - btn_back.press_start_us;
        
        if(press_duration_us >= LONGPRESS_THRESHOLD_US) {
            FURI_LOG_W(TAG, "Long-press detected on button key (pin %d), triggering shutdown...", BOARD_PIN_BUTTON_KEY);
            /* Trigger system shutdown (without emitting additional input events) */
            furi_hal_power_shutdown();
        }
    }
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
    btn->press_start_us = 0;  /* Not pressed initially */
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

    if(btn->debounced_pressed) { /* Button pressed → record timestamp for long-press detection */
        btn->press_start_us = esp_timer_get_time();
        input_publish(pubsub, key, InputTypePress, ++(*sequence_counter));
        return;
    }

    /* FIXED BUG: Hitung durasi rill penekanan sebelum mereset timestamp */
    int64_t release_time = esp_timer_get_time();
    int64_t actual_duration_us = release_time - btn_back.press_start_us; // Mengacu pada pin back pembawa shutdown
    
    btn->press_start_us = 0;
    input_publish(pubsub, key, InputTypeRelease, ++(*sequence_counter));

    /* Hanya tembakkan Short Click jika durasi tekan di bawah ambang batas LONGPRESS (2.5s)
     * Ini menjamin proses shutdown berjalan bersih tanpa interupsi menu GUI tambahan */
    if(actual_duration_us < LONGPRESS_THRESHOLD_US) {
        input_emit_short(pubsub, key, ++(*sequence_counter));
    } else {
        FURI_LOG_I(TAG, "Suppressed short click event due to ongoing long-press shutdown logic");
    }
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

        /* Setup long-press timer configuration */
    const esp_timer_create_args_t timer_args = {
        .callback = longpress_timer_callback,
        .name = "longpress_timer",
        .arg = NULL,
    };
    
    esp_err_t err = esp_timer_create(&timer_args, &longpress_timer);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "Failed to create long-press timer: %s", esp_err_to_name(err));
        return;
    }
    
    button_init_gpio((gpio_num_t)BOARD_PIN_BUTTON_KEY, true);
    button_init_state(&btn_back, (gpio_num_t)BOARD_PIN_BUTTON_KEY, true);

    if(btn_back.debounced_pressed) {
        btn_back.press_start_us = esp_timer_get_time();
    }

    /* BARU NYALAKAN TIMER setelah semua variabel status tombol bersih dan sinkron */
    esp_timer_start_periodic(longpress_timer, LONGPRESS_TIMER_PERIOD);
    FURI_LOG_I(TAG, "Long-press timer started (threshold: %ldms)", (long)BOARD_LONGPRESS_SHUTDOWN_MS);

    FURI_LOG_I(TAG, "Custom board input initialized with Boot-Safe Sequence");
}

void target_input_poll(FuriPubSub* pubsub, uint32_t* sequence_counter) {
    button_poll(&btn_up, pubsub, InputKeyUp, sequence_counter);
    button_poll(&btn_down, pubsub, InputKeyDown, sequence_counter);
    button_poll(&btn_left, pubsub, InputKeyLeft, sequence_counter);
    button_poll(&btn_right, pubsub, InputKeyRight, sequence_counter);
    button_poll(&btn_ok, pubsub, InputKeyOk, sequence_counter);
    button_poll(&btn_back, pubsub, InputKeyBack, sequence_counter);
}
