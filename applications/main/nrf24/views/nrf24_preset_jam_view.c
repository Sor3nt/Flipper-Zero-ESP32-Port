#include "nrf24_preset_jam_view.h"

#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <assets_icons.h>
#include <stdio.h>

static void nrf24_preset_jam_draw_callback(Canvas* canvas, void* _model) {
    Nrf24PresetJamModel* model = _model;
    canvas_clear(canvas);

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "Preset Jammer");

    if(!model->hardware_ok) {
        canvas_draw_icon(canvas, 60, 18, &I_Quest_7x8);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, "NRF24 not found");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignCenter, "Hardware fault?");
        return;
    }

    /* Preset name + strategy row, inverted while running. */
    char head[28];
    if(model->flooding) {
        snprintf(
            head, sizeof(head), "%s FLOOD%s", model->preset_name, model->low_rate ? " 250k" : "");
    } else {
        snprintf(head, sizeof(head), "%s CW", model->preset_name);
    }

    if(model->running) {
        canvas_draw_box(canvas, 4, 11, 121, 13);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 13, AlignCenter, AlignTop, head);
    canvas_set_color(canvas, ColorBlack);

    /* Channel / frequency. */
    char line[24];
    snprintf(line, sizeof(line), "CH %u  %u MHz", model->channel, 2400u + model->channel);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 27, AlignCenter, AlignTop, line);

    /* Dwell + hop counter. */
    snprintf(
        line,
        sizeof(line),
        "dwell %uus  hops:%lu",
        model->dwell_us,
        (unsigned long)model->hop_count);
    canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignTop, line);

    /* Button hints: Left=CW Right=Flood, OK=Run/Stop, Up/Down tune dwell. */
    elements_button_left(canvas, "CW");
    elements_button_right(canvas, "Flood");
    elements_button_center(canvas, model->running ? "Stop" : "Run");
}

static bool nrf24_preset_jam_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    bool repeatable = (event->type == InputTypeShort) || (event->type == InputTypeRepeat);

    switch(event->key) {
    case InputKeyOk:
        if(event->type == InputTypeShort) {
            view_dispatcher_send_custom_event(vd, Nrf24PresetJamEventToggle);
            return true;
        }
        return false;
    case InputKeyLeft:
        if(event->type != InputTypeShort) return false;
        view_dispatcher_send_custom_event(vd, Nrf24PresetJamEventToggleStrategy);
        return true;
    case InputKeyRight:
        if(event->type != InputTypeShort) return false;
        view_dispatcher_send_custom_event(vd, Nrf24PresetJamEventToggleRate);
        return true;
    case InputKeyUp:
        if(!repeatable) return false;
        view_dispatcher_send_custom_event(vd, Nrf24PresetJamEventDwellUp);
        return true;
    case InputKeyDown:
        if(!repeatable) return false;
        view_dispatcher_send_custom_event(vd, Nrf24PresetJamEventDwellDown);
        return true;
    default:
        return false;
    }
}

View* nrf24_preset_jam_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(Nrf24PresetJamModel));
    view_set_draw_callback(view, nrf24_preset_jam_draw_callback);
    view_set_input_callback(view, nrf24_preset_jam_input_callback);
    return view;
}

void nrf24_preset_jam_view_free(View* view) {
    view_free(view);
}
