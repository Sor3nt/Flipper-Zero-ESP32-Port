/**
 * @file target_input.c
 * Input driver for the smoochie board (ESP32-S3 N16R8)
 *
 * Buttons (5): Up / Down / Left / Right / Select(BOOT)
 *
 *   Up     short tap        -> Up
 *          hold 2.5 s       -> power shutdown
 *   Down   short tap        -> Down
 *          hold 0.8 s       -> Back (real Back *Short*, so ViewDispatcher/SceneManager
 *                              navigation works everywhere; Down itself is released first)
 *   Left / Right            -> short tap on release, fast repeat while held
 *   Select                  -> short tap on release
 *   Up + Left held 5 s      -> forced reboot, checked from a hardware timer ISR
 *
 * Shutdown is implemented the same way as on the paradoxt board:
 *   - a dedicated esp_timer (50 ms) watches the shutdown button, independent of the
 *     task that calls target_input_poll() (SPI/LCD/SD stalls cannot delay it)
 *   - the hold time is measured from a timestamp taken at the debounced press edge
 *   - a shutdown request is queued for the power service while the button is
 *     still held; the power service owns the BATFET/deep-sleep transition
 *   - "boot-safe": a button that is already held when target_input_init() runs
 *     (power-on via QON) is ignored until it has been released once
 *
 * Bugs fixed compared with the previous smoochie version:
 *   1. Down/Select/Left/Right latched a "long press" after 500 ms and then DROPPED the
 *      Short event on release -> any press longer than 0.5 s was silently lost. Only Up
 *      (1.5 s) tolerated slow presses, hence "only Up works". Short now fires on release
 *      unless a repeat / Back / shutdown already consumed the hold.
 *   2. Down-hold sent Back as Press+Long only. View dispatchers navigate back on
 *      InputTypeShort, so it worked only in views that handle Back-Long.
 *   3. Missing Release after long-press/repeat left the GUI's "ongoing input" state stale.
 *   4. press_started_at was 0 for a button held at boot -> huge hold time -> instant
 *      long-press/shutdown.
 */

#include "target_input.h"

#include <furi_hal_resources.h>
#include <furi_hal_power.h>
#include <boards/board.h>
#include <power/power_service/power.h>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_err.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>
#include <soc/gpio_struct.h>

#if CONFIG_FREERTOS_UNICORE || !CONFIG_ESP_TIMER_SHOW_EXPERIMENTAL || \
    !CONFIG_ESP_TIMER_ISR_AFFINITY_CPU1 || !CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD
#error "Smoochie forced reboot requires dual-core FreeRTOS and ESP Timer ISR on CPU1"
#endif

#define TAG "InputSmoochie"

/* ---- Tunables ---- */
#define INPUT_DEBOUNCE_MS      20U   /* raw level must be stable this long */
#define INPUT_DOWN_BACK_HOLD_MS 500U /* hold Down this long -> Back */
#define INPUT_REPEAT_START_MS  200U  /* Left/Right: hold before repeat starts */
#define INPUT_REPEAT_RATE_MS   100U  /* Left/Right: repeat interval */
#define REBOOT_COMBO_HOLD_MS   5000U /* Up + Left */

#define SHUTDOWN_TIMER_PERIOD_US 50000 /* 50 ms, same as paradoxt */
#define REBOOT_TIMER_PERIOD_US  100000 /* ISR sampling interval */
/* Keep enabled on Smoochie: it proves whether a wrong key originates at the
 * GPIO wiring or later in the input dispatcher. */
#define INPUT_DEBUG_EDGES 1

#define NUM_BUTTONS 5
#define BTN_UP      0
#define BTN_DOWN    1
#define BTN_LEFT    2

typedef struct {
    /* static config */
    gpio_num_t gpio;
    InputKey key;
    bool inverted;
    bool allow_repeat; /* Left/Right */
    bool hold_is_back; /* Down */
    bool is_shutdown;  /* Up */

    /* debounce (touched by the poll task only) */
    bool raw_pressed;
    uint32_t raw_changed_ms;

    /* shared with the shutdown timer -> volatile, 32-bit (atomic on Xtensa) */
    volatile bool debounced_pressed;
    volatile bool wait_release; /* held at init -> ignore until released once */
    volatile uint32_t press_start_ms;

    /* per-press state (poll task only) */
    uint32_t sequence;
    bool release_pending; /* a Press was published and its Release is still owed */
    bool hold_handled;    /* Down already turned into Back */
    bool repeat_active;
    uint32_t last_repeat_ms;
} ButtonState;

static ButtonState buttons[NUM_BUTTONS];
static ButtonState* shutdown_btn = NULL;
static esp_timer_handle_t shutdown_timer = NULL;
static bool shutdown_attempted = false;
static volatile bool shutdown_requested = false;

static esp_timer_handle_t reboot_timer = NULL;
static int64_t reboot_combo_started_us = 0;
static volatile bool reboot_combo_armed = false;
static volatile bool reboot_combo_wait_release = false;

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const char* input_key_name(InputKey key) {
    switch(key) {
    case InputKeyUp: return "Up";
    case InputKeyDown: return "Down";
    case InputKeyLeft: return "Left";
    case InputKeyRight: return "Right";
    case InputKeyOk: return "Ok";
    case InputKeyBack: return "Back";
    default: return "Other";
    }
}

static void input_publish(FuriPubSub* pubsub, InputKey key, InputType type, uint32_t sequence) {
    InputEvent event = {
        .sequence_source = INPUT_SEQUENCE_SOURCE_HARDWARE,
        .sequence_counter = sequence,
        .key = key,
        .type = type,
    };
    furi_pubsub_publish(pubsub, &event);
}

static bool button_is_pressed(ButtonState* btn) {
    int level = gpio_get_level(btn->gpio);
    return btn->inverted ? (level == 0) : (level != 0);
}

static bool IRAM_ATTR smoochie_gpio_is_low(gpio_num_t pin) {
    return (GPIO.in1.val & (1UL << (pin - 32))) == 0;
}

static void button_init_gpio(gpio_num_t pin, bool pull_up, bool pull_down) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull_down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "GPIO %d config failed: %s", (int)pin, esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------------ */
/* Shutdown timer (paradoxt-style)                                           */
/* ------------------------------------------------------------------------ */

static void shutdown_timer_callback(void* arg) {
    (void)arg;
    ButtonState* b = shutdown_btn;

    if(!b || !b->debounced_pressed || b->wait_release) {
        shutdown_attempted = false;
        return;
    }

    /* Up+Left is reserved for the forced reboot chord, not power-off. */
    if(button_is_pressed(&buttons[BTN_LEFT])) {
        shutdown_attempted = false;
        return;
    }

    uint32_t now = now_ms();
    uint32_t held = now - b->press_start_ms;

    if(held >= (uint32_t)BOARD_SHUTDOWN_TIME_MS) {
        if(!shutdown_attempted) {
            shutdown_attempted = true;
            FURI_LOG_W(
                TAG,
                "Long-press on shutdown button (pin %d) -> shutdown",
                (int)BOARD_SHUTDOWN_BUTTON);
            /* Defer the actual power transition to target_input_poll(). ESP
             * timer callbacks run on the esp_timer task; entering deep sleep
             * or touching I2C from that callback can reset instead of
             * completing the BQ25896 ship-mode sequence. */
            shutdown_requested = true;
        }
    }
}

static void IRAM_ATTR reboot_timer_callback(void* arg) {
    (void)arg;

    const bool up_pressed = smoochie_gpio_is_low((gpio_num_t)BOARD_PIN_BTN_UP);
    const bool left_pressed = smoochie_gpio_is_low((gpio_num_t)BOARD_PIN_BTN_LEFT);

    if(reboot_combo_wait_release) {
        if(!up_pressed && !left_pressed) reboot_combo_wait_release = false;
        reboot_combo_armed = false;
        reboot_combo_started_us = 0;
        return;
    }

    if(up_pressed && left_pressed) {
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
        .skip_unhandled_events = false,
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
            "Forced-reboot timer active (Up + Left, %u ms)",
            (unsigned)REBOOT_COMBO_HOLD_MS);
    }
}

/* ------------------------------------------------------------------------ */
/* Per-button polling                                                        */
/* ------------------------------------------------------------------------ */

static void button_held(
    ButtonState* btn,
    FuriPubSub* pubsub,
    uint32_t now,
    bool combo_active,
    uint32_t* sequence_counter) {
    uint32_t held = now - btn->press_start_ms;

    if(btn->hold_is_back) {
        if(!btn->hold_handled && held >= INPUT_DOWN_BACK_HOLD_MS) {
            btn->hold_handled = true; /* consumes this hold: no Up Short on release */

            /* Balance the Down Press, then deliver a complete Back click. */
            if(btn->release_pending) {
                input_publish(pubsub, btn->key, InputTypeRelease, btn->sequence);
                btn->release_pending = false;
            }
            uint32_t seq = ++(*sequence_counter);
            input_publish(pubsub, InputKeyBack, InputTypePress, seq);
            input_publish(pubsub, InputKeyBack, InputTypeShort, seq);
            input_publish(pubsub, InputKeyBack, InputTypeRelease, seq);
        }
        return;
    }

    if(btn->allow_repeat && !combo_active) {
        if(!btn->repeat_active) {
            if(held >= INPUT_REPEAT_START_MS) {
                btn->repeat_active = true;
                btn->last_repeat_ms = now;
                input_publish(pubsub, btn->key, InputTypeRepeat, btn->sequence);
            }
        } else if((uint32_t)(now - btn->last_repeat_ms) >= INPUT_REPEAT_RATE_MS) {
            btn->last_repeat_ms = now;
            input_publish(pubsub, btn->key, InputTypeRepeat, btn->sequence);
        }
    }
    /* Up / Select: nothing while held (Up's long hold belongs to the shutdown timer) */
}

static void button_poll(
    ButtonState* btn,
    FuriPubSub* pubsub,
    uint32_t now,
    bool combo_active,
    uint32_t* sequence_counter) {
    bool raw = button_is_pressed(btn);

    if(raw != btn->raw_pressed) {
        btn->raw_pressed = raw;
        btn->raw_changed_ms = now;
#if INPUT_DEBUG_EDGES
        FURI_LOG_I(
            TAG,
            "gpio %d raw -> %s (logical=%s)",
            (int)btn->gpio,
            raw ? "PRESSED" : "released",
            input_key_name(btn->key));
#endif
    }

    /* Time-based debounce: level must have been stable for INPUT_DEBOUNCE_MS. */
    if((uint32_t)(now - btn->raw_changed_ms) < INPUT_DEBOUNCE_MS) return;

    /* Held since boot: publish nothing until it has been released once. */
    if(btn->wait_release) {
        if(!btn->raw_pressed) {
            btn->debounced_pressed = false;
            btn->wait_release = false;
        }
        return;
    }

    if(btn->debounced_pressed == btn->raw_pressed) {
        if(btn->debounced_pressed) {
            button_held(btn, pubsub, now, combo_active, sequence_counter);
        }
        return;
    }

    if(btn->raw_pressed) {
        /* ---- debounced PRESS edge ---- */
        btn->press_start_ms = now;
        __sync_synchronize(); /* start time must be visible before the flag */
        btn->debounced_pressed = true;

        btn->hold_handled = false;
        btn->repeat_active = false;
        btn->sequence = ++(*sequence_counter);
        btn->release_pending = true;
        input_publish(pubsub, btn->key, InputTypePress, btn->sequence);
#if INPUT_DEBUG_EDGES
        FURI_LOG_I(
            TAG,
            "input press gpio=%d logical=%s",
            (int)btn->gpio,
            input_key_name(btn->key));
#endif
        return;
    }

    /* ---- debounced RELEASE edge ---- */
    btn->debounced_pressed = false;
    if(btn->is_shutdown) {
        /* A new long-press is allowed only after the shutdown button has
         * completed a release edge. This prevents a confirmation prompt from
         * being queued repeatedly while the button remains held. */
        shutdown_attempted = false;
    }
    uint32_t held = now - btn->press_start_ms;

    /* Short click on release unless something already consumed this hold:
       repeat (Left/Right), Back (Up), or a shutdown-length hold (Down). */
    bool suppress_short =
        btn->repeat_active || btn->hold_handled ||
        (btn->is_shutdown && held >= (uint32_t)BOARD_SHUTDOWN_TIME_MS);

    if(!suppress_short && btn->release_pending) {
        input_publish(pubsub, btn->key, InputTypeShort, btn->sequence);
    }
    if(btn->release_pending) {
        input_publish(pubsub, btn->key, InputTypeRelease, btn->sequence);
        btn->release_pending = false;
    }
#if INPUT_DEBUG_EDGES
    FURI_LOG_I(
        TAG,
        "input release gpio=%d logical=%s held=%lu ms back=%d",
        (int)btn->gpio,
        input_key_name(btn->key),
        (unsigned long)held,
        btn->hold_handled);
#endif

    btn->repeat_active = false;
    btn->hold_handled = false;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

void target_input_init(void) {
    const struct {
        gpio_num_t pin;
        InputKey key;
        bool allow_repeat;
        bool hold_is_back;
        bool pull_up;
        bool pull_down;
        bool inverted;
    } cfg[NUM_BUTTONS] = {
        {(gpio_num_t)BOARD_PIN_BTN_UP,      InputKeyUp,    false, false, true,  false, true },
        /* GPIO40 is driven by the AO3400/QON inverter and has an external pull-up. */
        {(gpio_num_t)BOARD_PIN_BTN_DOWN,    InputKeyDown,  false, true,  false, false, false},
        {(gpio_num_t)BOARD_PIN_BTN_LEFT,    InputKeyLeft,  true,  false, true,  false, true },
        {(gpio_num_t)BOARD_PIN_BTN_RIGHT,   InputKeyRight, true,  false, true,  false, true },
        {(gpio_num_t)BOARD_PIN_BUTTON_BOOT, InputKeyOk,    false, false, true,  false, true },
    };

    /* Re-init safety */
    if(shutdown_timer) {
        esp_timer_stop(shutdown_timer);
        esp_timer_delete(shutdown_timer);
        shutdown_timer = NULL;
    }
    if(reboot_timer) {
        esp_timer_stop(reboot_timer);
        esp_timer_delete(reboot_timer);
        reboot_timer = NULL;
    }
    shutdown_btn = NULL;

    uint32_t now = now_ms();

    for(int i = 0; i < NUM_BUTTONS; i++) {
        button_init_gpio(cfg[i].pin, cfg[i].pull_up, cfg[i].pull_down);

        ButtonState* b = &buttons[i];
        b->gpio = cfg[i].pin;
        b->key = cfg[i].key;
        b->inverted = cfg[i].inverted;
        b->allow_repeat = cfg[i].allow_repeat;
        b->hold_is_back = cfg[i].hold_is_back;
        b->is_shutdown = (cfg[i].pin == (gpio_num_t)BOARD_SHUTDOWN_BUTTON);

        b->raw_pressed = button_is_pressed(b);
        b->raw_changed_ms = now - INPUT_DEBOUNCE_MS; /* start out "stable" */
        b->debounced_pressed = b->raw_pressed;
        b->press_start_ms = now;
        b->wait_release = false;
        b->sequence = 0;
        b->release_pending = false;
        b->hold_handled = false;
        b->repeat_active = false;
        b->last_repeat_ms = 0;

        if(b->raw_pressed) {
            /* Boot-safe: a hold that began before init must not count. */
            b->wait_release = true;
            FURI_LOG_W(
                TAG, "gpio %d reads PRESSED at init -> ignored until released", (int)b->gpio);
        }

        if(b->is_shutdown) shutdown_btn = b;
    }

    shutdown_attempted = false;
    shutdown_requested = false;
    reboot_timer_init(buttons[BTN_UP].raw_pressed || buttons[BTN_LEFT].raw_pressed);

    FURI_LOG_I(
        TAG,
        "pins UP=%d DOWN=%d LEFT=%d RIGHT=%d OK=%d SHUTDOWN=%d",
        (int)BOARD_PIN_BTN_UP,
        (int)BOARD_PIN_BTN_DOWN,
        (int)BOARD_PIN_BTN_LEFT,
        (int)BOARD_PIN_BTN_RIGHT,
        (int)BOARD_PIN_BUTTON_BOOT,
        (int)BOARD_SHUTDOWN_BUTTON);

    if(!shutdown_btn) {
        FURI_LOG_E(
            TAG,
            "BOARD_SHUTDOWN_BUTTON (%d) matches none of the 5 button pins -> shutdown DISABLED",
            (int)BOARD_SHUTDOWN_BUTTON);
    } else {
        const esp_timer_create_args_t timer_args = {
            .callback = shutdown_timer_callback,
            .arg = NULL,
            .name = "shutdown_timer",
        };
        esp_err_t err = esp_timer_create(&timer_args, &shutdown_timer);
        if(err != ESP_OK) {
            FURI_LOG_E(TAG, "Failed to create shutdown timer: %s", esp_err_to_name(err));
            shutdown_timer = NULL;
        } else {
            /* Start only after every button state is clean and in sync. */
            err = esp_timer_start_periodic(shutdown_timer, SHUTDOWN_TIMER_PERIOD_US);
            if(err != ESP_OK) {
                FURI_LOG_E(TAG, "Failed to start shutdown timer: %s", esp_err_to_name(err));
            } else {
                FURI_LOG_I(
                    TAG,
                    "Shutdown timer started (pin %d, %ld ms)",
                    (int)BOARD_SHUTDOWN_BUTTON,
                    (long)BOARD_SHUTDOWN_TIME_MS);
            }
        }
    }

    FURI_LOG_I(TAG, "5-button input initialized (paradoxt-style shutdown, boot-safe)");
}

void target_input_poll(FuriPubSub* pubsub, uint32_t* sequence_counter) {
    uint32_t now = now_ms();

    if(shutdown_requested) {
        shutdown_requested = false;
        Power* power = furi_record_open(RECORD_POWER);
        power_show_poweroff_confirmation(power);
        furi_record_close(RECORD_POWER);
    }

    for(int i = 0; i < NUM_BUTTONS; i++) {
        button_poll(&buttons[i], pubsub, now, reboot_combo_armed, sequence_counter);
    }
}
