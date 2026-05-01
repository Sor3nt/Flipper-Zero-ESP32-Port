#include "ble_brute_view.h"
#include "../ble_brute_log.h"

#include <gui/canvas.h>
#include <gui/elements.h>
#include <input/input.h>
#include <stdio.h>

static void brute_draw_callback(Canvas* canvas, void* _model) {
    BleBruteModel* model = _model;
    canvas_clear(canvas);

    // Title with progress percent
    canvas_set_font(canvas, FontPrimary);
    char title[24];
    int pct = 0;
    if(model->total_handles > 0) {
        pct = (int)((100UL * model->handles_done) / model->total_handles);
    }
    snprintf(title, sizeof(title), "Brute Force %3d%%", pct);
    canvas_draw_str(canvas, 2, 10, title);
    canvas_draw_line(canvas, 0, 12, 128, 12);

    canvas_set_font(canvas, FontSecondary);

    if(model->failed) {
        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, "Connect failed");
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "Back to exit");
        return;
    }

    char line[40];
    if(model->target_name[0]) {
        snprintf(line, sizeof(line), "%.24s", model->target_name);
        canvas_draw_str(canvas, 2, 22, line);
        canvas_draw_str(canvas, 2, 31, model->target_addr);
    } else {
        canvas_draw_str(canvas, 2, 22, model->target_addr);
    }

    snprintf(line, sizeof(line), "Handle: 0x%04X  Hits: %u",
             model->current_handle, model->hit_count);
    canvas_draw_str(canvas, 2, 42, line);

    if(model->hit_count > 0) {
        snprintf(line, sizeof(line), "Last: 0x%04X %s %uB",
                 model->last_hit_handle,
                 ble_brute_status_name(model->last_hit_status),
                 model->last_hit_value_len);
        canvas_draw_str(canvas, 2, 51, line);
    }

    canvas_set_font(canvas, FontPrimary);
    if(model->done) {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "Done. Back to exit");
    } else {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "Back to stop");
    }
}

static bool brute_input_callback(InputEvent* event, void* context) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

View* ble_brute_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(BleBruteModel));
    view_set_draw_callback(view, brute_draw_callback);
    view_set_input_callback(view, brute_input_callback);
    return view;
}

void ble_brute_view_free(View* view) {
    view_free(view);
}
