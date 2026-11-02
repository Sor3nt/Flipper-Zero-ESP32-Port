#include "power_off.h"
#include <furi.h>
#include <gui/elements.h>
#include <assets_icons.h>

struct PowerOff {
    View* view;
};

typedef struct {
    PowerOffResponse response;
    uint32_t time_left_sec;
    bool manual;
    bool back_press_seen;
} PowerOffModel;

static void power_off_draw_callback(Canvas* canvas, void* _model) {
    furi_assert(_model);
    PowerOffModel* model = _model;
    char buff[32];

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas,
        64,
        1,
        AlignCenter,
        AlignTop,
        model->manual ? "Power off?" : "Battery low!");
    canvas_draw_icon(canvas, 0, 18, &I_BatteryBody_52x28);
    canvas_draw_icon(canvas, 16, 25, &I_FaceNopower_29x14);
    elements_bubble(canvas, 54, 17, 70, 30);

    canvas_set_font(canvas, FontSecondary);
    if(model->response == PowerOffResponseDefault) {
        if(model->manual) {
            snprintf(buff, sizeof(buff), "Turn device off?");
        } else {
            snprintf(buff, sizeof(buff), "Charge me!\nOff in %lus!", model->time_left_sec);
        }
        elements_multiline_text_aligned(canvas, 70, 23, AlignLeft, AlignTop, buff);

        elements_button_left(canvas, "Cancel");
        elements_button_center(canvas, "OK");
        elements_button_right(canvas, "Hide");
    } else {
        snprintf(
            buff,
            sizeof(buff),
            model->manual ? "Powering off..." : "Charge me!\nDon't forget!");
        elements_multiline_text_aligned(canvas, 70, 23, AlignLeft, AlignTop, buff);

        canvas_draw_str_aligned(canvas, 64, 60, AlignCenter, AlignBottom, "Hold a second...");
    }
}

static bool power_off_input_callback(InputEvent* event, void* context) {
    PowerOff* power_off = context;

    bool consumed = false;
    PowerOffModel* model = view_get_model(power_off->view);

    /* InputTypeRelease is used instead of InputTypeShort: Left/Right have
     * allow_repeat=true in target_input.c, so a press held past
     * INPUT_REPEAT_START_MS (200ms) becomes a Repeat and the terminating
     * Short is suppressed on release. Release is always published exactly
     * once per gesture regardless of hold duration, so it's the only event
     * type that reliably reaches this view for every tap length. */
    if(model->response == PowerOffResponseDefault && event->key == InputKeyBack &&
       event->type == InputTypePress) {
        model->back_press_seen = true;
    } else if(
        model->response == PowerOffResponseDefault && event->key == InputKeyBack &&
        event->type == InputTypeRelease) {
        /* The confirmation can open while Back is still held. Its eventual
         * release belongs to the gesture that opened this view, not a cancel. */
        if(model->back_press_seen) {
            model->response = PowerOffResponseCancel;
            consumed = true;
        }
        model->back_press_seen = false;
    } else if(model->response == PowerOffResponseDefault && event->type == InputTypeRelease) {
        if(event->key == InputKeyOk) {
            model->response = PowerOffResponseOk;
            consumed = true;
        } else if(event->key == InputKeyLeft) {
            model->response = PowerOffResponseCancel;
            consumed = true;
        } else if(event->key == InputKeyRight) {
            model->response = PowerOffResponseHide;
            consumed = true;
        }
    }
    view_commit_model(power_off->view, consumed);

    return consumed; /* fix: was unconditionally `true`, swallowing unhandled events */
}

PowerOff* power_off_alloc(void) {
    PowerOff* power_off = malloc(sizeof(PowerOff));

    power_off->view = view_alloc();
    view_allocate_model(power_off->view, ViewModelTypeLocking, sizeof(PowerOffModel));
    view_set_context(power_off->view, power_off);
    view_set_draw_callback(power_off->view, power_off_draw_callback);
    view_set_input_callback(power_off->view, power_off_input_callback);

    return power_off;
}

void power_off_free(PowerOff* power_off) {
    furi_assert(power_off);
    view_free(power_off->view);
    free(power_off);
}

View* power_off_get_view(PowerOff* power_off) {
    furi_assert(power_off);
    return power_off->view;
}

void power_off_set_time_left(PowerOff* power_off, uint8_t time_left) {
    furi_assert(power_off);
    with_view_model(
        power_off->view, PowerOffModel * model, { model->time_left_sec = time_left; }, true);
}

void power_off_set_manual(PowerOff* power_off, bool manual) {
    furi_assert(power_off);
    with_view_model(
        power_off->view, PowerOffModel * model, { model->manual = manual; }, true);
}

void power_off_reset_response(PowerOff* power_off) {
    furi_assert(power_off);
    with_view_model(
        power_off->view,
        PowerOffModel * model,
        {
            model->response = PowerOffResponseDefault;
            model->time_left_sec = 0;
            model->back_press_seen = false;
        },
        true);
}

PowerOffResponse power_off_get_response(PowerOff* power_off) {
    furi_assert(power_off);
    PowerOffResponse response;
    with_view_model(
        power_off->view, PowerOffModel * model, { response = model->response; }, false);
    return response;
}
