#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_pwm.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdio.h>

#include "servo_input.h"
#include "servo_pins.h"
#include "servotester_state.h"

#define TAG "ServoTester"

#define SERVOTESTER_FREQ 50
// Upstream: no point updating the PWM faster than 1.5 pulse periods (30 ms).
#define SERVOTESTER_AUTO_TICK_MS ((1000 / SERVOTESTER_FREQ) * 3 / 2)
// A full LCD frame takes 20-35 ms on the shared SPI bus, so the sweep is redrawn every
// third tick (~11 Hz) instead of every tick; the PWM itself still moves every tick.
#define SERVOTESTER_AUTO_REDRAW_TICKS 3

typedef enum {
    ServoTesterEventTypeTick,
    ServoTesterEventTypeInput,
} ServoTesterEventType;

typedef struct {
    ServoTesterEventType type;
    InputEvent input;
    uint32_t time_ms;
} ServoTesterEvent;

typedef enum {
    ServoTesterPowerOff, // 5V pin unpowered: the servo needs its own supply (common GND)
    ServoTesterPowerUsb, // USB VBUS feeds the 5V pin
    ServoTesterPowerOtg, // charger boost (OTG) feeds the 5V pin
} ServoTesterPower;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* event_queue;
    FuriMutex* mutex;
    FuriTimer* timer;

    ServoTesterState state;
    ServoInput input;
    uint8_t pin; // index into servo_pwm_ids
    bool output_ok; // PWM runs on the pin (false: pin busy, e.g. IR TX / USB-UART)
    bool otg_ours; // we turned OTG on, so we turn it off again
    ServoTesterPower power;
} ServoTesterApp;

static const char* const modes_text[ServoTesterModeCount] = {
    "Center",
    "Manual",
    " Auto",
};

static void servotester_draw_callback(Canvas* canvas, void* ctx) {
    ServoTesterApp* app = ctx;
    char temp_str[36];

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    const ServoTesterState state = app->state;
    const uint8_t pin = app->pin;
    const bool output_ok = app->output_ok;
    const ServoTesterPower power = app->power;
    furi_mutex_release(app->mutex);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);

    canvas_draw_frame(canvas, 0, 0, 128, 64);
    snprintf(temp_str, sizeof(temp_str), "%s pulse width", servo_pin_names[pin]);
    canvas_draw_str_aligned(canvas, 64, 10, AlignCenter, AlignBottom, temp_str);
    canvas_draw_line(canvas, 14, 30, 114, 30);
    canvas_draw_line(canvas, 14, 30, 14, 20);
    canvas_draw_line(canvas, 114, 30, 114, 20);

    // Upstream (width - MIN) / 10.2 + 14, in integers: 14..112 for 1000..2000 us.
    canvas_draw_frame(
        canvas, 14 + (state.width - SERVOTESTER_MIN_WIDTH) * 10 / 102, 20, 3, 10);

    snprintf(temp_str, sizeof(temp_str), "%u us", (unsigned)state.width);
    canvas_draw_str(canvas, 50, 40, temp_str);
    canvas_draw_str(canvas, 50, 50, modes_text[state.mode]);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 3, 61, output_ok ? "Hold OK: pin" : "Pin busy!");
    const char* power_text = power == ServoTesterPowerOtg ? "5V:OTG" :
                             power == ServoTesterPowerUsb ? "5V:USB" :
                                                            "5V:off";
    canvas_draw_str_aligned(canvas, 125, 61, AlignRight, AlignBottom, power_text);
}

static uint32_t servotester_time_ms(void) {
    return (uint32_t)((uint64_t)furi_get_tick() * 1000 / furi_kernel_get_tick_frequency());
}

static void servotester_input_callback(InputEvent* input_event, void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* event_queue = ctx;
    ServoTesterEvent event = {
        .type = ServoTesterEventTypeInput,
        .input = *input_event,
        .time_ms = servotester_time_ms(),
    };
    // Bounded wait: this runs on the GUI thread, which must never park on a full queue.
    furi_message_queue_put(event_queue, &event, 50);
}

static void servotester_timer_callback(void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* event_queue = ctx;

    ServoTesterEvent event = {.type = ServoTesterEventTypeTick};
    furi_message_queue_put(event_queue, &event, 0);
}

/* Starts the line low (duty 0) and then sets the pulse, so the very first pulse already
 * has the selected width. Leaves output_ok false when the HAL refuses the pin. */
static void servotester_output_start(ServoTesterApp* app) {
    const FuriHalPwmOutputId id = servo_pwm_ids[app->pin];
    furi_hal_pwm_start(id, SERVOTESTER_FREQ, 0);
    app->output_ok = furi_hal_pwm_is_running(id) &&
                     furi_hal_pwm_set_pulse(id, SERVOTESTER_FREQ, app->state.width);
    if(!app->output_ok) {
        furi_hal_pwm_stop(id);
        FURI_LOG_W(TAG, "%s is busy", servo_pin_names[app->pin]);
    }
}

static void servotester_output_stop(ServoTesterApp* app) {
    if(app->output_ok) furi_hal_pwm_stop(servo_pwm_ids[app->pin]);
    app->output_ok = false;
}

static void servotester_output_update(ServoTesterApp* app) {
    if(app->output_ok) {
        furi_hal_pwm_set_pulse(servo_pwm_ids[app->pin], SERVOTESTER_FREQ, app->state.width);
    }
}

static void servotester_power_on(ServoTesterApp* app) {
    if(furi_hal_power_is_otg_enabled()) {
        // Already on (GPIO > VBUS 5V): not ours to turn off.
        app->power = ServoTesterPowerOtg;
    } else if(!furi_hal_power_is_running_on_battery()) {
        // USB already feeds the 5V pin, and OTG would stop charging (CHG_CONFIG=0).
        app->power = ServoTesterPowerUsb;
    } else {
        app->otg_ours = true;
        app->power = furi_hal_power_enable_otg() ? ServoTesterPowerOtg : ServoTesterPowerOff;
    }
}

static void servotester_power_off(ServoTesterApp* app) {
    if(app->otg_ours) furi_hal_power_disable_otg();
    app->otg_ours = false;
}

static void servotester_mode_changed(ServoTesterApp* app, ServoTesterMode old_mode) {
    if(app->state.mode == ServoTesterModeAuto) {
        furi_timer_start(app->timer, furi_ms_to_ticks(SERVOTESTER_AUTO_TICK_MS));
    } else if(old_mode == ServoTesterModeAuto) {
        furi_timer_stop(app->timer);
    }
}

int32_t servotester_app(void* p) {
    UNUSED(p);

    ServoTesterApp* app = malloc(sizeof(ServoTesterApp));
    servotester_state_init(&app->state);
    servo_input_reset(&app->input);
    app->pin = 0;
    app->output_ok = false;
    app->otg_ours = false;
    app->power = ServoTesterPowerOff;

    app->event_queue = furi_message_queue_alloc(16, sizeof(ServoTesterEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, servotester_draw_callback, app);
    view_port_input_callback_set(app->view_port, servotester_input_callback, app->event_queue);

    // Timer for automatic mode, only running while in Auto
    app->timer =
        furi_timer_alloc(servotester_timer_callback, FuriTimerTypePeriodic, app->event_queue);

    // Turn 5V on first, then the signal
    servotester_power_on(app);
    servotester_output_start(app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    ServoTesterEvent event;
    uint8_t auto_ticks = 0;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app->event_queue, &event, FuriWaitForever) != FuriStatusOk) {
            continue;
        }

        bool redraw = false;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        const uint16_t old_width = app->state.width;
        const ServoTesterMode old_mode = app->state.mode;

        if(event.type == ServoTesterEventTypeInput) {
            InputEvent input;
            if(servo_input_translate(&app->input, &event.input, event.time_ms, &input)) {
                const ServoTesterAction action =
                    servotester_on_key(&app->state, input.key, input.type);
                if(action == ServoTesterActionExit) {
                    running = false;
                } else if(action == ServoTesterActionNextPin) {
                    servotester_output_stop(app);
                    app->pin = (app->pin + 1) % SERVO_PIN_COUNT;
                    servotester_output_start(app);
                }
                redraw = true;
            }
        } else if(servotester_on_tick(&app->state)) {
            if(++auto_ticks >= SERVOTESTER_AUTO_REDRAW_TICKS) {
                auto_ticks = 0;
                redraw = true;
            }
        }

        if(running) {
            if(app->state.width != old_width) servotester_output_update(app);
            if(app->state.mode != old_mode) {
                servotester_mode_changed(app, old_mode);
                auto_ticks = 0;
                redraw = true;
            }
        }
        furi_mutex_release(app->mutex);

        if(running && redraw) view_port_update(app->view_port);
    }

    // No more ticks, then stop the signal first and power the component off second
    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);
    servotester_output_stop(app);
    servotester_power_off(app);

    view_port_enabled_set(app->view_port, false);
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    furi_message_queue_free(app->event_queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}
