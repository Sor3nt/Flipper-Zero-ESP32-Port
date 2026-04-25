#include "handshake_channel_view.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static void hs_channel_view_draw_callback(Canvas* canvas, void* _model) {
    HsChannelViewModel* model = _model;
    canvas_clear(canvas);

    // Header
    canvas_set_font(canvas, FontPrimary);
    char header[32];
    snprintf(header, sizeof(header), "Ch:%d Wifi:%d | HS:%d",
             model->channel, model->count, model->hs_complete_count);
    canvas_draw_str(canvas, 2, 10, header);
    canvas_draw_line(canvas, 0, 12, 128, 12);

    if(model->count == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter,
                                model->running ? "Listening..." : "Idle");
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "Up/Dn:Channel");
        return;
    }

    canvas_set_font(canvas, FontSecondary);
    int line_height = 12;
    int header_height = 14;

    for(int i = 0; i < HS_CHANNEL_ITEMS_ON_SCREEN && (model->window_offset + i) < model->count; i++) {
        int idx = model->window_offset + i;
        HsChannelEntry* e = &model->entries[idx];
        int y = header_height + i * line_height;

        // Selection highlight
        if(idx == model->selected) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y, 128, line_height);
            canvas_set_color(canvas, ColorWhite);
        }

        // SSID (max 5 chars)
        canvas_draw_str(canvas, 2, y + 10, e->ssid);

        // M1-M4 + B indicators (only show if captured)
        int x = 65;
        if(e->has_m1) { canvas_draw_str(canvas, x, y + 10, "1"); }
        x += 10;
        if(e->has_m2) { canvas_draw_str(canvas, x, y + 10, "2"); }
        x += 10;
        if(e->has_m3) { canvas_draw_str(canvas, x, y + 10, "3"); }
        x += 10;
        if(e->has_m4) { canvas_draw_str(canvas, x, y + 10, "4"); }
        x += 10;
        if(e->has_beacon) { canvas_draw_str(canvas, x, y + 10, "B"); }

        // Checkmark if complete
        if(e->complete) {
            canvas_draw_str(canvas, 115, y + 10, "\x01"); // tick char, fallback below
            // Draw a simple checkmark manually
            int cx = 117, cy = y + 3;
            canvas_draw_line(canvas, cx, cy + 4, cx + 2, cy + 6);
            canvas_draw_line(canvas, cx + 2, cy + 6, cx + 6, cy);
        }

        // Reset color
        if(idx == model->selected) {
            canvas_set_color(canvas, ColorBlack);
        }
    }

    // Scrollbar
    if(model->count > HS_CHANNEL_ITEMS_ON_SCREEN) {
        elements_scrollbar(canvas, model->selected, model->count);
    }
}

static bool hs_channel_view_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    if(event->key == InputKeyUp || event->key == InputKeyDown) {
        // Scroll handled internally — but also forward to scene for potential use
        // Actually, just forward to scene; scene updates model
        view_dispatcher_send_custom_event(vd, event->key);
        return true;
    }
    if(event->key == InputKeyLeft || event->key == InputKeyRight) {
        // Channel change
        view_dispatcher_send_custom_event(vd, event->key);
        return true;
    }

    return false;
}

View* handshake_channel_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(HsChannelViewModel));
    view_set_draw_callback(view, hs_channel_view_draw_callback);
    view_set_input_callback(view, hs_channel_view_input_callback);
    return view;
}

void handshake_channel_view_free(View* view) {
    view_free(view);
}
