#include "nrf24_smart_jam_view.h"

#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <assets_icons.h>
#include <stdio.h>

static void draw_scan_phase(Canvas* canvas, Nrf24SmartJamModel* model) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "Smart Jammer");
    canvas_draw_line(canvas, 0, 13, 127, 13);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignTop, "Scanning 2.4 GHz...");

    /* Progress-Bar */
    const int bar_x = 8;
    const int bar_y = 36;
    const int bar_w = 112;
    const int bar_h = 9;
    canvas_draw_frame(canvas, bar_x, bar_y, bar_w, bar_h);
    int fill = (bar_w - 2) * model->scan_progress / 100;
    if(fill > 0) canvas_draw_box(canvas, bar_x + 1, bar_y + 1, fill, bar_h - 2);

    char info[24];
    snprintf(info, sizeof(info), "%u sweeps  %u%%", model->sweep_count, model->scan_progress);
    canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignTop, info);
}

static void draw_jam_smart(Canvas* canvas, Nrf24SmartJamModel* model) {
    if(model->target_count == 0) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, "No active");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "channels - try Wide");
        return;
    }

    /* Channel grid: 4 columns x 2 rows for up to 8 targets */
    canvas_set_font(canvas, FontSecondary);
    const int col_w = 32;
    const int row_h = 17;
    const int grid_y = 16;

    for(uint8_t i = 0; i < model->target_count; i++) {
        int col = i % 4;
        int row = i / 4;
        int x = col * col_w;
        int y = grid_y + row * row_h;

        bool is_current = model->running && (i == model->current_target_idx);
        if(is_current) {
            canvas_draw_box(canvas, x, y, col_w, row_h - 1);
            canvas_set_color(canvas, ColorWhite);
        }

        char chbuf[8];
        snprintf(chbuf, sizeof(chbuf), "Ch%u", model->targets[i]);
        canvas_draw_str(canvas, x + 2, y + 7, chbuf);

        char hitbuf[8];
        snprintf(hitbuf, sizeof(hitbuf), "%u", model->target_hits[i]);
        canvas_draw_str(canvas, x + 2, y + 14, hitbuf);

        if(is_current) canvas_set_color(canvas, ColorBlack);
    }
}

static void draw_jam_wide(Canvas* canvas, Nrf24SmartJamModel* model) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 18, AlignCenter, AlignTop, "WIDE SWEEP");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignTop, "2400 - 2479 MHz");

    char info[24];
    snprintf(
        info,
        sizeof(info),
        "Ch %u  hops %u",
        model->current_channel,
        (unsigned)model->hop_count);
    canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignTop, info);
}

static void draw_jam_phase(Canvas* canvas, Nrf24SmartJamModel* model) {
    /* Header */
    canvas_set_font(canvas, FontPrimary);
    char header[24];
    if(model->wide_mode) {
        snprintf(header, sizeof(header), "Wide Jam [80]");
    } else {
        snprintf(header, sizeof(header), "Smart Jam [%u]", model->target_count);
    }
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, header);
    canvas_draw_line(canvas, 0, 13, 127, 13);

    if(model->wide_mode) {
        draw_jam_wide(canvas, model);
    } else {
        draw_jam_smart(canvas, model);
    }

    /* Footer button hints */
    canvas_set_font(canvas, FontSecondary);
    elements_button_center(canvas, model->running ? "Stop" : "Run");
    elements_button_left(canvas, model->wide_mode ? "Smart" : "Wide");
}

static void nrf24_smart_jam_draw_callback(Canvas* canvas, void* _model) {
    Nrf24SmartJamModel* model = _model;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    if(!model->hardware_ok) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "Smart Jammer");
        canvas_draw_line(canvas, 0, 13, 127, 13);
        canvas_draw_icon(canvas, 60, 22, &I_Quest_7x8);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "NRF24 not found");
        return;
    }

    if(model->phase == SmartJamPhaseScan) {
        draw_scan_phase(canvas, model);
    } else {
        draw_jam_phase(canvas, model);
    }
}

static bool nrf24_smart_jam_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type != InputTypeShort) return false;

    switch(event->key) {
    case InputKeyOk:
        view_dispatcher_send_custom_event(vd, Nrf24SmartJamEventToggle);
        return true;
    case InputKeyLeft:
    case InputKeyRight:
    case InputKeyUp:
    case InputKeyDown:
        view_dispatcher_send_custom_event(vd, Nrf24SmartJamEventToggleMode);
        return true;
    default:
        return false;
    }
}

View* nrf24_smart_jam_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(Nrf24SmartJamModel));
    view_set_draw_callback(view, nrf24_smart_jam_draw_callback);
    view_set_input_callback(view, nrf24_smart_jam_input_callback);
    return view;
}

void nrf24_smart_jam_view_free(View* view) {
    view_free(view);
}
