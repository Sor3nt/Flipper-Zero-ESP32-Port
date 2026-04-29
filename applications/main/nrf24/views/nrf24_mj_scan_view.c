#include "nrf24_mj_scan_view.h"

#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <assets_icons.h>
#include <stdio.h>
#include <string.h>

static void nrf24_mj_scan_draw_callback(Canvas* canvas, void* _model) {
    Nrf24MjScanModel* model = _model;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    /* Header */
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "MouseJacker Scan");
    canvas_draw_line(canvas, 0, 13, 127, 13);

    if(!model->hardware_ok) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_icon(canvas, 60, 22, &I_Quest_7x8);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "NRF24 not found");
        return;
    }

    canvas_set_font(canvas, FontSecondary);

    /* Status line */
    char status[32];
    snprintf(
        status,
        sizeof(status),
        "Ch %3u  Targets %u/16",
        model->current_channel,
        model->target_count);
    canvas_draw_str_aligned(canvas, 64, 18, AlignCenter, AlignTop, status);

    /* Sweep indicator */
    char sw[24];
    snprintf(sw, sizeof(sw), "%u sweeps", (unsigned)model->sweep_count);
    canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignTop, sw);

    /* Last hit */
    if(model->last_target_label[0]) {
        canvas_draw_line(canvas, 4, 40, 123, 40);
        canvas_draw_str_aligned(canvas, 64, 43, AlignCenter, AlignTop, "Last:");
        canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignTop, model->last_target_label);
    } else {
        canvas_draw_str_aligned(canvas, 64, 43, AlignCenter, AlignTop, "Listening…");
    }

    /* Footer hint */
    elements_button_center(canvas, model->target_count > 0 ? "List" : "Stop");
}

static bool nrf24_mj_scan_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type != InputTypeShort) return false;
    if(event->key == InputKeyOk) {
        view_dispatcher_send_custom_event(vd, Nrf24MjScanEventStop);
        return true;
    }
    return false;
}

View* nrf24_mj_scan_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(Nrf24MjScanModel));
    view_set_draw_callback(view, nrf24_mj_scan_draw_callback);
    view_set_input_callback(view, nrf24_mj_scan_input_callback);
    return view;
}

void nrf24_mj_scan_view_free(View* view) {
    view_free(view);
}
