#include "nrf24_ch_jammer_view.h"

#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <assets_icons.h>
#include <stdio.h>

static void nrf24_ch_jammer_draw_callback(Canvas* canvas, void* _model) {
    Nrf24ChJammerModel* model = _model;
    canvas_clear(canvas);

    /* Title */
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "CH Jammer");

    if(!model->hardware_ok) {
        canvas_draw_icon(canvas, 60, 18, &I_Quest_7x8);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, "NRF24 not found");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignCenter, "Hardware fault?");
        return;
    }

    /* Big channel number with optional inverted background */
    char freq[8];
    snprintf(freq, sizeof(freq), "%03lu.0", 2400ul + model->channel);

    if(model->running) {
        canvas_draw_box(canvas, 4, 10, 121, 19);
        canvas_set_color(canvas, ColorWhite);
    }

    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str(canvas, 8, 26, freq);
    canvas_draw_icon(canvas, 96, 15, &I_MHz_25x11);

    canvas_set_color(canvas, ColorBlack);

    /* Channel + status row */
    char chbuf[16];
    snprintf(chbuf, sizeof(chbuf), "Channel %u", model->channel);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 35, AlignCenter, AlignTop, chbuf);

//    canvas_set_font(canvas, FontPrimary);
//    canvas_draw_str_aligned(
//        canvas, 64, 50, AlignCenter, AlignTop, model->running ? "JAMMING" : "Standby");

    /* Bottom button hints */
    canvas_set_font(canvas, FontSecondary);
    elements_button_left(canvas, "CH-");
    elements_button_right(canvas, "CH+");
    elements_button_center(canvas, model->running ? "Stop" : "Run");
}

static bool nrf24_ch_jammer_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    switch(event->key) {
    case InputKeyOk:
        if(event->type == InputTypeShort) {
            view_dispatcher_send_custom_event(vd, Nrf24ChJammerEventToggle);
            return true;
        }
        return false;
    case InputKeyRight:
    case InputKeyUp:
        view_dispatcher_send_custom_event(vd, Nrf24ChJammerEventChannelUp);
        return true;
    case InputKeyLeft:
    case InputKeyDown:
        view_dispatcher_send_custom_event(vd, Nrf24ChJammerEventChannelDown);
        return true;
    default:
        return false;
    }
}

View* nrf24_ch_jammer_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(Nrf24ChJammerModel));
    view_set_draw_callback(view, nrf24_ch_jammer_draw_callback);
    view_set_input_callback(view, nrf24_ch_jammer_input_callback);
    return view;
}

void nrf24_ch_jammer_view_free(View* view) {
    view_free(view);
}
