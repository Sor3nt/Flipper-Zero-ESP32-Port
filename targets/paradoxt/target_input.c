/**
 * @file target_input.c
 * Input driver for the custom board using dedicated button pins.
 *
 * Features:
 *   - 6-way button pad (Up/Down/Left/Right) + Select/OK (BOOT pin)
 *   - Back button (pin 21); holding it 2.5s is handled by the Desktop, which
 *     opens the power-off dialog (Power app with arg "off")
 *   - Back button also wired to BQ25896 QON for power-on wake
 *   - Debouncing (2 polls)
 *   - Repeat-while-held for Up/Down/Left/Right/OK (fast scroll)
 *   - Up + Down held for 5s triggers an unconditional reboot
 */

#include "target_input.h"

#include <furi_hal_resources.h>
#include <furi_hal_power.h>
#include <furi.h>
#include <boards/board.h>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_err.h>
#include <esp_rom_sys.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <soc/gpio_struct.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#if CONFIG_FREERTOS_UNICORE || !CONFIG_ESP_TIMER_SHOW_EXPERIMENTAL || \
    !CONFIG_ESP_TIMER_ISR_AFFINITY_CPU1 || !CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD
#error "Paradoxt forced reboot requires dual-core FreeRTOS and ESP Timer ISR on CPU1"
#endif

#define TAG "InputCustom"

#define INPUT_DEBOUNCE_POLLS    2U
#define LONGPRESS_THRESHOLD_US  (BOARD_LONGPRESS_SHUTDOWN_MS * 1000)

#define REPEAT_START_US  200000
#define REPEAT_RATE_US   100000
#define REBOOT_COMBO_HOLD_MS 5000U
#define REBOOT_TIMER_PERIOD_US 100000

typedef struct {
    gpio_num_t gpio;
    bool inverted;
    bool raw_pressed;
    bool debounced_pressed;
    bool wait_release;
    uint8_t debounce_polls;
    int64_t press_start_us;
    bool repeat_active;
    int64_t last_repeat_us;
} ButtonState;

static ButtonState btn_up;
static ButtonState btn_down;
static ButtonState btn_left;
static ButtonState btn_right;
static ButtonState btn_ok;
static ButtonState btn_back;
static esp_timer_handle_t reboot_timer;
static SemaphoreHandle_t idle_wake_sem;
static StaticSemaphore_t idle_wake_sem_buffer;
static bool idle_wake_ready;
static int64_t reboot_combo_started_us;
static volatile bool reboot_combo_armed;
static volatile bool reboot_combo_wait_release;

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
    input_publish(pubsub, key, InputTypeShort, sequence);
}

static bool button_is_pressed(ButtonState* btn) {
    int level = gpio_get_level(btn->gpio);
    return btn->inverted ? (level == 0) : (level != 0);
}

static bool IRAM_ATTR paradoxt_gpio_is_low(gpio_num_t pin) {
    return (GPIO.in1.val & (1UL << (pin - 32))) == 0;
}

static void IRAM_ATTR reboot_timer_callback(void* arg) {
    (void)arg;

    const bool up_pressed = paradoxt_gpio_is_low((gpio_num_t)BOARD_PIN_BTN_UP);
    const bool down_pressed = paradoxt_gpio_is_low((gpio_num_t)BOARD_PIN_BTN_DOWN);

    if(reboot_combo_wait_release) {
        if(!up_pressed && !down_pressed) reboot_combo_wait_release = false;
        reboot_combo_armed = false;
        reboot_combo_started_us = 0;
        return;
    }

    if(up_pressed && down_pressed) {
        if(!reboot_combo_armed) {
            reboot_combo_armed = true;
            reboot_combo_started_us = esp_timer_get_time();
        } else if(esp_timer_get_time() - reboot_combo_started_us >=
                  (int64_t)REBOOT_COMBO_HOLD_MS * 1000) {
            esp_rom_software_reset_system();
            __builtin_unreachable();
        }
    } else {
        reboot_combo_armed = false;
        reboot_combo_started_us = 0;
    }
}

static void reboot_timer_init(bool wait_for_release) {
    reboot_combo_wait_release = wait_for_release;
    reboot_combo_armed = false;
    reboot_combo_started_us = 0;

    const esp_timer_create_args_t timer_args = {
        .callback = reboot_timer_callback,
        .dispatch_method = ESP_TIMER_ISR,
        /* Do not wake the SoC from light sleep 10x per second. While asleep no
         * button is held (a press wakes the SoC through the GPIO wake source
         * and the display), so missed periods are irrelevant for the combo. */
        .skip_unhandled_events = true,
        .name = "forced_reboot",
    };
    esp_err_t err = esp_timer_create(&timer_args, &reboot_timer);
    if(err == ESP_OK) err = esp_timer_start_periodic(reboot_timer, REBOOT_TIMER_PERIOD_US);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "Failed to initialize forced-reboot timer: %s", esp_err_to_name(err));
        if(reboot_timer) {
            esp_timer_delete(reboot_timer);
            reboot_timer = NULL;
        }
    } else {
        FURI_LOG_I(
            TAG,
            "Forced-reboot timer active (Up + Down, %u ms)",
            (unsigned)REBOOT_COMBO_HOLD_MS);
    }
}

static void button_init_gpio(gpio_num_t pin, bool pull_up, bool pull_down) {
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull_down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
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
    btn->wait_release = btn->raw_pressed;
    btn->debounce_polls = INPUT_DEBOUNCE_POLLS;
    btn->press_start_us = btn->raw_pressed ? esp_timer_get_time() : 0;
    btn->repeat_active = false;
    btn->last_repeat_us = 0;
}

static void button_poll(
    ButtonState* btn,
    FuriPubSub* pubsub,
    InputKey key,
    bool allow_repeat,
    bool combo_active,
    uint32_t* sequence_counter)
{
    bool raw = button_is_pressed(btn);

    if(raw == btn->raw_pressed) {
        if(btn->debounce_polls < INPUT_DEBOUNCE_POLLS) {
            btn->debounce_polls++;
        }
    } else {
        btn->raw_pressed = raw;
        btn->debounce_polls = 1;
        if(raw && !btn->debounced_pressed && btn == &btn_back) {
            btn->press_start_us = esp_timer_get_time();
        }
    }

    if(btn->debounce_polls < INPUT_DEBOUNCE_POLLS) return;

    if(btn->wait_release) {
        if(!btn->raw_pressed) {
            btn->debounced_pressed = false;
            btn->wait_release = false;
            btn->press_start_us = 0;
            btn->repeat_active = false;
        }
        return;
    }

    if(btn->debounced_pressed == btn->raw_pressed) {
        if(btn->debounced_pressed && allow_repeat && !combo_active) {
            int64_t now = esp_timer_get_time();
            int64_t held_us = now - btn->press_start_us;

            if(!btn->repeat_active && held_us >= REPEAT_START_US) {
                btn->repeat_active = true;
                btn->last_repeat_us = now;
                input_publish(pubsub, key, InputTypeRepeat, ++(*sequence_counter));
            } else if(btn->repeat_active && (now - btn->last_repeat_us) >= REPEAT_RATE_US) {
                btn->last_repeat_us = now;
                input_publish(pubsub, key, InputTypeRepeat, ++(*sequence_counter));
            }
        }
        return;
    }

    const bool pressed = btn->raw_pressed;
    if(pressed && !btn->press_start_us) btn->press_start_us = esp_timer_get_time();
    btn->debounced_pressed = pressed;

    if(pressed) {
        btn->repeat_active = false;
        input_publish(pubsub, key, InputTypePress, ++(*sequence_counter));
        return;
    }

    int64_t release_time = esp_timer_get_time();
    int64_t actual_duration_us = release_time - btn->press_start_us;

    bool was_repeating = btn->repeat_active;
    btn->press_start_us = 0;
    btn->repeat_active = false;

    input_publish(pubsub, key, InputTypeRelease, ++(*sequence_counter));

    if(!was_repeating && actual_duration_us < LONGPRESS_THRESHOLD_US) {
        input_emit_short(pubsub, key, ++(*sequence_counter));
    } else if(was_repeating) {
        FURI_LOG_D(TAG, "Held+released after repeat, skipping short click");
    } else {
        FURI_LOG_I(TAG, "Suppressed short click event due to ongoing long-press shutdown logic");
    }
}

/* ---- Light-sleep wake on buttons ----
 * While the power service has granted light sleep the input service blocks in
 * target_input_idle_wait() instead of polling every 20 ms. Each button pin is
 * a level-triggered GPIO wake source and interrupt: the first press wakes the
 * SoC, the ISR masks the pins and releases the input thread, and normal
 * debounced polling takes over again. */
typedef struct {
    ButtonState* button;
    gpio_int_type_t active_level;
} IdleWakePin;

static IdleWakePin idle_wake_pins[] = {
    {&btn_up, GPIO_INTR_LOW_LEVEL},
    {&btn_down, GPIO_INTR_LOW_LEVEL},
    {&btn_left, GPIO_INTR_LOW_LEVEL},
    {&btn_right, GPIO_INTR_LOW_LEVEL},
    {&btn_ok, GPIO_INTR_LOW_LEVEL},
    {&btn_back, GPIO_INTR_HIGH_LEVEL}, /* active-high via the QON inverter */
};

static void idle_wake_isr(void* arg) {
    (void)arg;
    for(size_t i = 0; i < COUNT_OF(idle_wake_pins); i++) {
        gpio_intr_disable(idle_wake_pins[i].button->gpio);
    }
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(idle_wake_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

static void idle_wake_init(void) {
    idle_wake_sem = xSemaphoreCreateBinaryStatic(&idle_wake_sem_buffer);
    const esp_err_t isr_err = gpio_install_isr_service(0);
    if(isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        FURI_LOG_E(TAG, "GPIO ISR service unavailable: %s", esp_err_to_name(isr_err));
        return;
    }
    for(size_t i = 0; i < COUNT_OF(idle_wake_pins); i++) {
        const gpio_num_t pin = idle_wake_pins[i].button->gpio;
        gpio_intr_disable(pin);
        if(gpio_isr_handler_add(pin, idle_wake_isr, NULL) != ESP_OK) {
            FURI_LOG_E(TAG, "GPIO%d wake handler failed; input keeps polling", pin);
            return;
        }
    }
    if(esp_sleep_enable_gpio_wakeup() != ESP_OK) {
        FURI_LOG_E(TAG, "GPIO light-sleep wakeup unavailable; input keeps polling");
        return;
    }
    idle_wake_ready = true;
}

static bool input_buttons_idle(void) {
    for(size_t i = 0; i < COUNT_OF(idle_wake_pins); i++) {
        const ButtonState* btn = idle_wake_pins[i].button;
        if(btn->raw_pressed || btn->debounced_pressed || btn->wait_release ||
           btn->debounce_polls < INPUT_DEBOUNCE_POLLS) {
            return false;
        }
    }
    return !reboot_combo_armed;
}

bool target_input_idle_wait(uint32_t timeout_ms) {
    /* A press, release, repeat or long-press in progress needs polling. */
    if(!idle_wake_ready || !input_buttons_idle()) return false;

    xSemaphoreTake(idle_wake_sem, 0); /* drop a stale wake */
    for(size_t i = 0; i < COUNT_OF(idle_wake_pins); i++) {
        const gpio_num_t pin = idle_wake_pins[i].button->gpio;
        /* gpio_wakeup_enable() also keeps the run-mode pull-up on the pad
         * during sleep (sleep_sel off), so the pin never floats. */
        gpio_wakeup_enable(pin, idle_wake_pins[i].active_level);
        gpio_intr_enable(pin);
    }
    /* Level-triggered: a press that happened after input_buttons_idle()
     * fires immediately, nothing is lost. */
    xSemaphoreTake(
        idle_wake_sem,
        timeout_ms == FuriWaitForever ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));
    for(size_t i = 0; i < COUNT_OF(idle_wake_pins); i++) {
        const gpio_num_t pin = idle_wake_pins[i].button->gpio;
        gpio_intr_disable(pin);
        gpio_wakeup_disable(pin);
    }
    return true;
}

void target_input_init(void) {
    if(reboot_timer) {
        esp_timer_stop(reboot_timer);
        esp_timer_delete(reboot_timer);
        reboot_timer = NULL;
    }

    button_init_gpio((gpio_num_t)BOARD_PIN_BTN_UP, true, false);
    button_init_state(&btn_up, (gpio_num_t)BOARD_PIN_BTN_UP, true);

    button_init_gpio((gpio_num_t)BOARD_PIN_BTN_DOWN, true, false);
    button_init_state(&btn_down, (gpio_num_t)BOARD_PIN_BTN_DOWN, true);

    button_init_gpio((gpio_num_t)BOARD_PIN_BTN_LEFT, true, false);
    button_init_state(&btn_left, (gpio_num_t)BOARD_PIN_BTN_LEFT, true);

    button_init_gpio((gpio_num_t)BOARD_PIN_BTN_RIGHT, true, false);
    button_init_state(&btn_right, (gpio_num_t)BOARD_PIN_BTN_RIGHT, true);

    button_init_gpio((gpio_num_t)BOARD_PIN_BUTTON_BOOT, true, false);
    button_init_state(&btn_ok, (gpio_num_t)BOARD_PIN_BUTTON_BOOT, true);

    button_init_gpio((gpio_num_t)BOARD_SHUTDOWN_BUTTON, false, false);
    button_init_state(&btn_back, (gpio_num_t)BOARD_SHUTDOWN_BUTTON, false);
    reboot_timer_init(btn_up.raw_pressed || btn_down.raw_pressed);
    if(!idle_wake_ready) idle_wake_init();

    FURI_LOG_I(
        TAG,
        "Custom board input initialized; hold Back (GPIO%d) for %ldms for the power-off dialog",
        BOARD_SHUTDOWN_BUTTON,
        (long)BOARD_LONGPRESS_SHUTDOWN_MS);
}

void target_input_poll(FuriPubSub* pubsub, uint32_t* sequence_counter) {
    const bool combo_active =
        button_is_pressed(&btn_up) && button_is_pressed(&btn_down);
    button_poll(&btn_up, pubsub, InputKeyUp, true, combo_active, sequence_counter);
    button_poll(&btn_down, pubsub, InputKeyDown, true, combo_active, sequence_counter);
    button_poll(&btn_left, pubsub, InputKeyLeft, true, false, sequence_counter);
    button_poll(&btn_right, pubsub, InputKeyRight, true, false, sequence_counter);
    button_poll(&btn_ok, pubsub, InputKeyOk, false, false, sequence_counter);
    /* Back long-press -> power-off dialog is owned by the Desktop (its 2.5 s
     * poweroff_timer launches "Power" with arg "off"). Launching Power here too
     * opened the Power settings list first and left the Desktop's request as a
     * deferred launch that only showed after Back closed the list. */
    button_poll(&btn_back, pubsub, InputKeyBack, false, false, sequence_counter);
}
