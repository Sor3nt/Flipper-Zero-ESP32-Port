#include "handshake_view.h"
#include <gui/canvas.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>
#include <string.h>

static void handshake_view_draw_callback(Canvas* canvas, void* _model) {
    HandshakeViewModel* model = _model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Handshake Capture");
    canvas_draw_line(canvas, 0, 12, 128, 12);

    canvas_set_font(canvas, FontSecondary);

    char buf[48];

    // Target info
    snprintf(buf, sizeof(buf), "%s  Ch:%d", model->ssid, model->channel);
    canvas_draw_str(canvas, 2, 23, buf);

    // M1-M4 + Beacon indicators
    const char* labels[] = {"M1", "M2", "M3", "M4", "B"};
    const bool* flags[] = {
        &model->has_m1, &model->has_m2, &model->has_m3,
        &model->has_m4, &model->has_beacon};

    for(int i = 0; i < 5; i++) {
        int x = 2 + i * 25;
        int y = 25;
        if(*flags[i]) {
            canvas_draw_box(canvas, x, y, 22, 11);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_str_aligned(canvas, x + 11, y + 9, AlignCenter, AlignBottom, labels[i]);
            canvas_set_color(canvas, ColorBlack);
        } else {
            canvas_draw_frame(canvas, x, y, 22, 11);
            canvas_draw_str_aligned(canvas, x + 11, y + 9, AlignCenter, AlignBottom, labels[i]);
        }
    }

    // Counters
    snprintf(buf, sizeof(buf), "EAPOL:%lu  Deauth:%lu",
             (unsigned long)model->eapol_count, (unsigned long)model->deauth_frames);
    canvas_draw_str(canvas, 2, 48, buf);

    // Timer
    uint32_t m = model->elapsed_sec / 60;
    uint32_t s = model->elapsed_sec % 60;
    snprintf(buf, sizeof(buf), "Time: %lu:%02lu", (unsigned long)m, (unsigned long)s);
    canvas_draw_str(canvas, 2, 58, buf);

    // Bottom bar
    canvas_set_font(canvas, FontPrimary);
    if(model->complete) {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "COMPLETE! Saved.");
    } else if(model->running) {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom,
                                model->deauth_active ? "OK:Stop  Up:NoDeauth" : "OK:Stop  Up:Deauth");
    } else {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK: Start");
    }
}

static bool handshake_view_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyOk || event->key == InputKeyUp) {
            view_dispatcher_send_custom_event(vd, event->key);
            return true;
        }
    }
    return false;
}

View* handshake_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(HandshakeViewModel));
    view_set_draw_callback(view, handshake_view_draw_callback);
    view_set_input_callback(view, handshake_view_input_callback);
    return view;
}

void handshake_view_free(View* view) {
    view_free(view);
}
