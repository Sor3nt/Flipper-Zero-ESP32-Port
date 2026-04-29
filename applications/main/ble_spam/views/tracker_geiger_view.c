#include "tracker_geiger_view.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static const char* proximity_label(int8_t rssi, bool stale) {
    if(stale) return "NO SIGNAL";
    if(rssi > -45) return "RIGHT HERE";
    if(rssi > -60) return "HOT";
    if(rssi > -72) return "WARM";
    if(rssi > -84) return "COOL";
    return "COLD";
}

static void tracker_geiger_draw_callback(Canvas* canvas, void* _model) {
    TrackerGeigerModel* model = _model;
    canvas_clear(canvas);

    // Header: "HUNT  AA:BB:CC:DD:EE:FF"
    canvas_set_font(canvas, FontPrimary);
    char title[40];
    snprintf(
        title,
        sizeof(title),
        "HUNT %02X:%02X:%02X:%02X:%02X:%02X",
        model->target_addr[0],
        model->target_addr[1],
        model->target_addr[2],
        model->target_addr[3],
        model->target_addr[4],
        model->target_addr[5]);
    canvas_draw_str(canvas, 2, 10, title);
    canvas_draw_line(canvas, 0, 12, 128, 12);

    // Proximity label (large, centered)
    canvas_set_font(canvas, FontPrimary);
    const char* label = proximity_label(model->rssi, model->stale);
    canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, label);

    // RSSI value
    canvas_set_font(canvas, FontSecondary);
    char rssi_text[16];
    if(model->stale) {
        snprintf(rssi_text, sizeof(rssi_text), "--- dBm");
    } else {
        snprintf(rssi_text, sizeof(rssi_text), "%d dBm", model->rssi);
    }
    canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignCenter, rssi_text);

    // RSSI bargraph: -100 dBm = empty, -30 dBm = full
    const uint8_t bar_x = 4;
    const uint8_t bar_y = 54;
    const uint8_t bar_w = 120;
    const uint8_t bar_h = 7;
    elements_frame(canvas, bar_x, bar_y, bar_w, bar_h);
    if(!model->stale) {
        int range = model->rssi + 100; // -100..-30 → 0..70
        if(range < 0) range = 0;
        if(range > 70) range = 70;
        uint8_t fill_w = (uint8_t)((range * (bar_w - 2)) / 70);
        if(fill_w > 0) {
            canvas_draw_box(canvas, bar_x + 1, bar_y + 1, fill_w, bar_h - 2);
        }
    }
}

static bool tracker_geiger_input_callback(InputEvent* event, void* context) {
    UNUSED(context);
    UNUSED(event);
    // Back is handled by view_dispatcher's navigation callback. No other input.
    return false;
}

View* tracker_geiger_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(TrackerGeigerModel));
    view_set_draw_callback(view, tracker_geiger_draw_callback);
    view_set_input_callback(view, tracker_geiger_input_callback);
    return view;
}

void tracker_geiger_view_free(View* view) {
    view_free(view);
}
