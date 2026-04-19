#include "deauther_view.h"
#include <gui/canvas.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static void deauther_view_draw_callback(Canvas* canvas, void* _model) {
    DeautherModel* model = _model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, model->channel_mode ? "Channel Deauth" : "Deauther");
    canvas_draw_line(canvas, 0, 12, 128, 12);

    canvas_set_font(canvas, FontSecondary);

    char buf[64];

    if(model->channel_mode) {
        snprintf(buf, sizeof(buf), "Channel: %d", model->channel);
        canvas_draw_str(canvas, 2, 24, buf);

        if(model->scanned) {
            snprintf(buf, sizeof(buf), "APs found: %d", model->ap_count);
            canvas_draw_str(canvas, 2, 34, buf);
        }

        snprintf(buf, sizeof(buf), "Frames: %lu", (unsigned long)model->frames_sent);
        canvas_draw_str(canvas, 2, 44, buf);
    } else {
        const char* ssid = model->ssid[0] ? model->ssid : "(hidden)";
        snprintf(buf, sizeof(buf), "Target: %.20s", ssid);
        canvas_draw_str(canvas, 2, 24, buf);

        snprintf(buf, sizeof(buf), "BSSID: %02X:%02X:%02X:%02X:%02X:%02X",
                 model->bssid[0], model->bssid[1], model->bssid[2],
                 model->bssid[3], model->bssid[4], model->bssid[5]);
        canvas_draw_str(canvas, 2, 34, buf);

        snprintf(buf, sizeof(buf), "Channel: %d", model->channel);
        canvas_draw_str(canvas, 2, 44, buf);

        snprintf(buf, sizeof(buf), "Frames: %lu", (unsigned long)model->frames_sent);
        canvas_draw_str(canvas, 2, 54, buf);
    }

    canvas_set_font(canvas, FontPrimary);
    if(model->channel_mode) {
        if(model->running) {
            canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK: Stop");
        } else if(model->scanned && model->ap_count > 0) {
            canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK: Start");
        } else {
            canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK: Scan");
        }
    } else {
        if(model->running) {
            canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK: Stop");
        } else {
            canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK: Start");
        }
    }
}

static bool deauther_view_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyOk) {
            view_dispatcher_send_custom_event(vd, InputKeyOk);
            return true;
        }
        if(event->key == InputKeyUp) {
            view_dispatcher_send_custom_event(vd, InputKeyUp);
            return true;
        }
        if(event->key == InputKeyDown) {
            view_dispatcher_send_custom_event(vd, InputKeyDown);
            return true;
        }
    }
    return false;
}

View* deauther_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(DeautherModel));
    view_set_draw_callback(view, deauther_view_draw_callback);
    view_set_input_callback(view, deauther_view_input_callback);
    return view;
}

void deauther_view_free(View* view) {
    view_free(view);
}
