#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_pwm.h>
#include <input/input.h>
#include <gui/gui.h>
#include <stdio.h>
#include <stdlib.h>

#include "servo_input.h"
#include "servo_pins.h"
#include "servotesterapp_state.h"

// ESP32 port: upstream wrote the STM32 TIM1 registers directly; the pulse now comes from
// furi_hal_pwm (LEDC) on a header pin. Build: ./buildFap.sh applications_user/servotesterapp

#define TAG "ServoTesterApp"

#define SCREEN_XRES 128
#define SCREEN_YRES 64
#define FREQ        50
#define SWEEP_MS    1000

typedef enum {
    EventTypeInput,
    EventTypeTick,
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
    uint32_t time_ms;
} AppEvent;

typedef struct App {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* event_queue;
    FuriMutex* mutex;
    FuriTimer* timer;

    ServoAppState state;
    ServoInput input;
    uint8_t pin; // index into servo_pwm_ids
    bool output_ok; // PWM runs on the pin (false: pin busy, e.g. IR TX / USB-UART)
    bool otg_ours; // we turned OTG on, so we turn it off again
} App;

static uint32_t app_time_ms(void) {
    return (uint32_t)((uint64_t)furi_get_tick() * 1000 / furi_kernel_get_tick_frequency());
}

static void input_callback(InputEvent* input_event, void* ctx) {
    App* app = ctx;
    AppEvent event = {.type = EventTypeInput, .input = *input_event, .time_ms = app_time_ms()};
    // Bounded wait: this runs on the GUI thread, which must never park on a full queue.
    furi_message_queue_put(app->event_queue, &event, 50);
}

/* Timer thread only posts an event: the state is owned by the app thread (upstream changed
 * the angle and the PWM straight from the timer callback). */
static void tick(void* ctx) {
    App* app = ctx;
    AppEvent event = {.type = EventTypeTick};
    furi_message_queue_put(app->event_queue, &event, 0);
}

static void render_callback(Canvas* const canvas, void* ctx) {
    App* app = ctx;

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    const ServoAppState state = app->state;
    const uint8_t pin = app->pin;
    const bool output_ok = app->output_ok;
    furi_mutex_release(app->mutex);

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 0, 0, SCREEN_XRES - 1, SCREEN_YRES - 1);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    uint8_t mode_base_x = 20;
    canvas_draw_str(canvas, mode_base_x, 10, "Mode");
    canvas_draw_str(canvas, mode_base_x, 30, "Manual");
    canvas_draw_str(canvas, mode_base_x, 45, "Center");
    canvas_draw_str(canvas, mode_base_x, 60, "Sweep");

    uint8_t selector_base_x = 5;
    uint8_t r = 4;
    uint8_t correction = r;
    if(state.mode == Manual) {
        canvas_draw_disc(canvas, selector_base_x, 30 - correction, r);
    } else {
        canvas_draw_circle(canvas, selector_base_x, 30 - correction, r);
    }

    if(state.mode == Center) {
        canvas_draw_disc(canvas, selector_base_x, 45 - correction, r);
    } else {
        canvas_draw_circle(canvas, selector_base_x, 45 - correction, r);
    }

    if(state.mode == Sweep) {
        canvas_draw_disc(canvas, selector_base_x, 60 - correction, r);
    } else {
        canvas_draw_circle(canvas, selector_base_x, 60 - correction, r);
    }

    uint8_t angle_base_x = 70;
    canvas_draw_str(canvas, angle_base_x, 10, "Angle");
    canvas_draw_circle(canvas, angle_base_x + 18, 21, 2);

    char angle_str[32];
    snprintf(angle_str, sizeof(angle_str), "%3u", state.angle);
    canvas_draw_str(canvas, angle_base_x, 30, angle_str);

    snprintf(angle_str, sizeof(angle_str), "%lu us", (unsigned long)angle_to_pulse_us(state.angle));
    canvas_draw_str(canvas, angle_base_x, 45, angle_str);

    snprintf(
        angle_str, sizeof(angle_str), output_ok ? "OK>%s" : "%s busy", servo_pin_names[pin]);
    canvas_draw_str(canvas, angle_base_x, 60, angle_str);
}

/* Starts the line low (duty 0) and then sets the pulse, so the very first pulse already
 * has the selected width. Leaves output_ok false when the HAL refuses the pin. */
static void output_start(App* app) {
    const FuriHalPwmOutputId id = servo_pwm_ids[app->pin];
    furi_hal_pwm_start(id, FREQ, 0);
    app->output_ok = furi_hal_pwm_is_running(id) &&
                     furi_hal_pwm_set_pulse(id, FREQ, angle_to_pulse_us(app->state.angle));
    if(!app->output_ok) {
        furi_hal_pwm_stop(id);
        FURI_LOG_W(TAG, "%s is busy", servo_pin_names[app->pin]);
    }
}

static void output_stop(App* app) {
    if(app->output_ok) furi_hal_pwm_stop(servo_pwm_ids[app->pin]);
    app->output_ok = false;
}

static void output_update(App* app) {
    if(app->output_ok) {
        furi_hal_pwm_set_pulse(servo_pwm_ids[app->pin], FREQ, angle_to_pulse_us(app->state.angle));
    }
}

/* Same 5V policy as the ServoTester FAP: OTG only on battery (USB VBUS already feeds the
 * pin, and OTG would stop charging), and never turn off an OTG someone else turned on. */
static void power_on(App* app) {
    if(!furi_hal_power_is_otg_enabled() && furi_hal_power_is_running_on_battery()) {
        app->otg_ours = true;
        furi_hal_power_enable_otg();
    }
}

static void power_off(App* app) {
    if(app->otg_ours) furi_hal_power_disable_otg();
    app->otg_ours = false;
}

App* app_alloc() {
    App* app = malloc(sizeof(App));

    servo_app_state_init(&app->state);
    servo_input_reset(&app->input);
    app->pin = 0;
    app->output_ok = false;
    app->otg_ours = false;

    app->event_queue = furi_message_queue_alloc(16, sizeof(AppEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->timer = furi_timer_alloc(tick, FuriTimerTypePeriodic, app);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, render_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);

    return app;
}

void app_free(App* app) {
    furi_assert(app);

    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);
    output_stop(app);
    power_off(app);

    view_port_enabled_set(app->view_port, false);
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->event_queue);
    furi_mutex_free(app->mutex);
    app->gui = NULL;

    free(app);
}

int32_t servotester_app_entry(void* p) {
    UNUSED(p);

    App* app = app_alloc();

    // 5V first, then the signal; the screen comes up once the pin state is known
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    power_on(app);
    output_start(app);
    furi_mutex_release(app->mutex);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    AppEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app->event_queue, &event, FuriWaitForever) != FuriStatusOk) {
            continue;
        }

        bool redraw = false;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        const ServoAppState old = app->state;

        if(event.type == EventTypeInput) {
            InputEvent input;
            if(servo_input_translate(&app->input, &event.input, event.time_ms, &input)) {
                const ServoAppAction action = servo_app_on_key(&app->state, input.key, input.type);
                if(action == ServoAppActionExit) {
                    running = false;
                } else if(action == ServoAppActionNextPin) {
                    output_stop(app);
                    app->pin = (app->pin + 1) % SERVO_PIN_COUNT;
                    output_start(app);
                    redraw = true;
                }
            }
        } else {
            servo_app_on_tick(&app->state);
        }

        if(running) {
            // Every angle change reaches the pin right away (upstream left the old pulse
            // running after entering Sweep until the first tick).
            if(app->state.angle != old.angle) output_update(app);
            if(app->state.mode != old.mode) {
                if(app->state.mode == Sweep) {
                    furi_timer_start(app->timer, furi_ms_to_ticks(SWEEP_MS));
                } else if(old.mode == Sweep) {
                    furi_timer_stop(app->timer);
                }
            }
            if(app->state.angle != old.angle || app->state.mode != old.mode) redraw = true;
        }
        furi_mutex_release(app->mutex);

        if(running && redraw) view_port_update(app->view_port);
    }

    app_free(app);

    return 0;
}
