#include "sniffer_view.h"
#include <gui/canvas.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static void sniffer_view_draw_callback(Canvas* canvas, void* _model) {
    SnifferViewModel* model = _model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Sniffer");
    canvas_draw_line(canvas, 0, 12, 128, 12);

    canvas_set_font(canvas, FontSecondary);

    char buf[48];

    snprintf(buf, sizeof(buf), "Channel: %d", model->channel);
    canvas_draw_str(canvas, 2, 24, buf);

    snprintf(buf, sizeof(buf), "Packets: %lu", (unsigned long)model->packets);
    canvas_draw_str(canvas, 2, 34, buf);

    snprintf(buf, sizeof(buf), "Bytes: %lu", (unsigned long)model->bytes);
    canvas_draw_str(canvas, 2, 44, buf);

    uint32_t m = model->elapsed_sec / 60;
    uint32_t s = model->elapsed_sec % 60;
    snprintf(buf, sizeof(buf), "Time: %lu:%02lu", (unsigned long)m, (unsigned long)s);
    canvas_draw_str(canvas, 2, 54, buf);

    canvas_set_font(canvas, FontPrimary);
    if(model->running) {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK: Stop");
    } else {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK: Start");
    }
}

static bool sniffer_view_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyOk || event->key == InputKeyUp ||
           event->key == InputKeyDown) {
            view_dispatcher_send_custom_event(vd, event->key);
            return true;
        }
    }
    return false;
}

View* sniffer_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(SnifferViewModel));
    view_set_draw_callback(view, sniffer_view_draw_callback);
    view_set_input_callback(view, sniffer_view_input_callback);
    return view;
}

void sniffer_view_free(View* view) {
    view_free(view);
}
