#include "ble_auto_walk_view.h"

#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static const char* status_text(AutoWalkStatus s) {
    switch(s) {
    case AutoWalkStatusIdle:     return "IDLE";
    case AutoWalkStatusScan:     return "SCAN";
    case AutoWalkStatusConnect:  return "CONNECT";
    case AutoWalkStatusDiscover: return "DISCOVER";
    case AutoWalkStatusRead:     return "READ";
    case AutoWalkStatusDone:     return "DONE";
    }
    return "?";
}

static void auto_walk_draw_callback(Canvas* canvas, void* _model) {
    BleAutoWalkModel* model = _model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Auto Walk");
    canvas_draw_line(canvas, 0, 12, 128, 12);

    canvas_set_font(canvas, FontSecondary);

    char line[40];
    snprintf(line, sizeof(line), "Seen: %u%s",
             model->seen_count, model->seen_full ? " (FULL)" : "");
    canvas_draw_str(canvas, 2, 22, line);

    if(model->last_name[0] || model->last_addr[0]) {
        snprintf(line, sizeof(line), "Last: %.24s",
                 model->last_name[0] ? model->last_name : model->last_addr);
        canvas_draw_str(canvas, 2, 32, line);

        if(model->last_name[0] && model->last_addr[0]) {
            canvas_draw_str(canvas, 8, 41, model->last_addr);
        }

        snprintf(line, sizeof(line), "%u svc / %u char",
                 model->last_services, model->last_chars);
        canvas_draw_str(canvas, 2, 50, line);
    } else {
        canvas_draw_str(canvas, 2, 36, "Waiting for devices...");
    }

    snprintf(line, sizeof(line), "Now: %s", status_text(model->status));
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, line);
}

static bool auto_walk_input_callback(InputEvent* event, void* context) {
    UNUSED(context);
    UNUSED(event);
    // Back is handled by the navigation callback at the dispatcher level.
    return false;
}

View* ble_auto_walk_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(BleAutoWalkModel));
    view_set_draw_callback(view, auto_walk_draw_callback);
    view_set_input_callback(view, auto_walk_input_callback);
    return view;
}

void ble_auto_walk_view_free(View* view) {
    view_free(view);
}
