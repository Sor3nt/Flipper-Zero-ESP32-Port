#include "airsnitch_view.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static void airsnitch_view_draw_callback(Canvas* canvas, void* _model) {
    AirSnitchViewModel* model = _model;
    canvas_clear(canvas);

    // Header
    canvas_set_font(canvas, FontPrimary);
    char header[32];
    if(model->scanning) {
        snprintf(header, sizeof(header), "Scanning... %d found", model->count);
    } else {
        snprintf(header, sizeof(header), "ARP Scan: %d hosts", model->count);
    }
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

    for(int i = 0; i < AIRSNITCH_ITEMS_ON_SCREEN &&
                   (model->window_offset + i) < model->count; i++) {
        int idx = model->window_offset + i;
        AirSnitchHost* h = &model->hosts[idx];
        int y = header_height + i * line_height;

        // Selection highlight
        if(idx == model->selected) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y, 128, line_height);
            canvas_set_color(canvas, ColorWhite);
        }

        // IP address (last octet only to save space) + full MAC
        uint8_t* ip = (uint8_t*)&h->ip;
        char line[40];
        snprintf(line, sizeof(line), "%d.%d.%d.%d %02X:%02X:%02X:%02X:%02X:%02X",
                 ip[0], ip[1], ip[2], ip[3],
                 h->mac[0], h->mac[1], h->mac[2],
                 h->mac[3], h->mac[4], h->mac[5]);
        canvas_draw_str(canvas, 2, y + 10, line);

        // Reset color
        if(idx == model->selected) {
            canvas_set_color(canvas, ColorBlack);
        }
    }

    // Scrollbar
    if(model->count > AIRSNITCH_ITEMS_ON_SCREEN) {
        elements_scrollbar(canvas, model->selected, model->count);
    }
}

static bool airsnitch_view_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    if(event->key == InputKeyUp || event->key == InputKeyDown) {
        view_dispatcher_send_custom_event(vd, event->key);
        return true;
    }

    return false;
}

View* airsnitch_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(AirSnitchViewModel));
    view_set_draw_callback(view, airsnitch_view_draw_callback);
    view_set_input_callback(view, airsnitch_view_input_callback);
    return view;
}

void airsnitch_view_free(View* view) {
    view_free(view);
}
