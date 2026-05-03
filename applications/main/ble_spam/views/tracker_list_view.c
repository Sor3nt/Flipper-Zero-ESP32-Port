#include "tracker_list_view.h"
#include "../ble_spam_app.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static void tracker_list_draw_callback(Canvas* canvas, void* _model) {
    TrackerListModel* model = _model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    char title[32];
    snprintf(title, sizeof(title), "Tracker (%d)", model->count);
    canvas_draw_str(canvas, 2, 10, title);
    canvas_draw_line(canvas, 0, 12, 128, 12);

    canvas_set_font(canvas, FontSecondary);

    if(model->count == 0) {
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "Scanning...");
    }

    for(int i = 0; i < TRACKER_LIST_ITEMS_ON_SCREEN && (model->window_offset + i) < model->count;
        i++) {
        uint16_t idx = model->window_offset + i;
        TrackerDevice* d = &model->devices[idx];

        // Pick label: model name if known (Apple PP), else generic kind label
        const char* label = d->name[0] ? d->name : ble_tracker_kind_label(d->kind);

        char line[40];
        // Format: "<label> XX:XX  -45/-43"  (last 2 MAC bytes, current/best RSSI)
        snprintf(
            line,
            sizeof(line),
            "%-12.12s %02X:%02X %d/%d",
            label,
            d->addr[4],
            d->addr[5],
            d->rssi,
            d->best_rssi);

        uint8_t y = 22 + i * 11;
        if(idx == model->selected) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y - 8, 128, 11);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, 2, y, line);
        if(idx == model->selected) {
            canvas_set_color(canvas, ColorBlack);
        }
    }

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK:Hunt");
}

static bool tracker_list_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyOk || event->key == InputKeyUp || event->key == InputKeyDown) {
            view_dispatcher_send_custom_event(vd, event->key);
            return true;
        }
    }
    return false;
}

View* tracker_list_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(TrackerListModel));
    view_set_draw_callback(view, tracker_list_draw_callback);
    view_set_input_callback(view, tracker_list_input_callback);
    return view;
}

void tracker_list_view_free(View* view) {
    view_free(view);
}
