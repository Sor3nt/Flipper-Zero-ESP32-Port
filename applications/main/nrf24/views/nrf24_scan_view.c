#include "nrf24_scan_view.h"

#include <gui/canvas.h>
#include <gui/elements.h>
#include <input/input.h>
#include <assets_icons.h>
#include <stdio.h>

static void nrf24_scan_draw_callback(Canvas* canvas, void* _model) {
    Nrf24ScanModel* model = _model;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 4, AlignCenter, AlignTop, "Activity Scan");

    if(!model->hardware_ok) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "NRF24 not found");
        return;
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignTop, "Sniffing 2.4 GHz...");

    /* Progress bar. */
    uint8_t p = model->progress > 100 ? 100 : model->progress;
    elements_progress_bar(canvas, 8, 36, 112, (float)p / 100.0f);

    char line[32];
    snprintf(line, sizeof(line), "%u%%   sweeps: %lu", p, (unsigned long)model->sweeps);
    canvas_draw_str_aligned(canvas, 64, 50, AlignCenter, AlignTop, line);
}

static bool nrf24_scan_input_callback(InputEvent* event, void* context) {
    UNUSED(event);
    UNUSED(context);
    /* No custom input — Back is handled by the view dispatcher navigation. */
    return false;
}

View* nrf24_scan_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(Nrf24ScanModel));
    view_set_draw_callback(view, nrf24_scan_draw_callback);
    view_set_input_callback(view, nrf24_scan_input_callback);
    return view;
}

void nrf24_scan_view_free(View* view) {
    view_free(view);
}
