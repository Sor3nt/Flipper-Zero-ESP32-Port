#include "netscan_view.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static void netscan_view_draw_callback(Canvas* canvas, void* _model) {
    NetscanViewModel* model = _model;
    canvas_clear(canvas);

    /* Header: own IP */
    canvas_set_font(canvas, FontPrimary);
    char header[40];
    snprintf(header, sizeof(header), "My IP: %s", model->own_ip);
    canvas_draw_str(canvas, 2, 10, header);
    canvas_draw_line(canvas, 0, 12, 128, 12);

    if(model->count == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter,
                                model->scanning ? model->status : "No hosts found");
        return;
    }

    canvas_set_font(canvas, FontSecondary);
    int line_height = 12;
    int header_height = 14;

    for(int i = 0; i < NETSCAN_ITEMS_ON_SCREEN &&
                   (model->window_offset + i) < model->count; i++) {
        int idx = model->window_offset + i;
        NetscanHost* h = &model->hosts[idx];
        int y = header_height + i * line_height;

        if(idx == model->selected) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y, 128, line_height);
            canvas_set_color(canvas, ColorWhite);
        }

        uint8_t* ip = (uint8_t*)&h->ip;
        char line[20];
        snprintf(line, sizeof(line), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
        canvas_draw_str(canvas, 2, y + 10, line);

        if(idx == model->selected) {
            canvas_set_color(canvas, ColorBlack);
        }
    }

    if(model->count > NETSCAN_ITEMS_ON_SCREEN) {
        elements_scrollbar(canvas, model->selected, model->count);
    }

    /* Bottom hint */
    canvas_set_font(canvas, FontSecondary);
    if(!model->scanning && model->count > 0) {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK: Port Scan");
    }
}

static bool netscan_view_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    if(event->key == InputKeyUp || event->key == InputKeyDown || event->key == InputKeyOk) {
        view_dispatcher_send_custom_event(vd, event->key);
        return true;
    }

    return false;
}

View* netscan_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(NetscanViewModel));
    view_set_draw_callback(view, netscan_view_draw_callback);
    view_set_input_callback(view, netscan_view_input_callback);
    return view;
}

void netscan_view_free(View* view) {
    view_free(view);
}
