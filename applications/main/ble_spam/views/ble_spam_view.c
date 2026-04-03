#include "ble_spam_view.h"
#include "../ble_spam_app.h"
#include <gui/canvas.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static void ble_spam_view_draw_callback(Canvas* canvas, void* _model) {
    BleSpamRunningModel* model = _model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, model->attack_name);
    canvas_draw_line(canvas, 0, 12, 128, 12);

    canvas_set_font(canvas, FontSecondary);

    char buf[64];

    const char* dev = model->device_name[0] ? model->device_name : "---";
    snprintf(buf, sizeof(buf), "Device: %.22s", dev);
    canvas_draw_str(canvas, 2, 24, buf);

    snprintf(buf, sizeof(buf), "Packets: %lu", (unsigned long)model->packet_count);
    canvas_draw_str(canvas, 2, 34, buf);

    snprintf(buf, sizeof(buf), "Speed: %lums", (unsigned long)model->delay_ms);
    canvas_draw_str(canvas, 2, 44, buf);

    canvas_set_font(canvas, FontPrimary);
    if(model->running) {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK:Pause  ^v:Speed");
    } else {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK: Start");
    }
}

static bool ble_spam_view_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyOk) {
            view_dispatcher_send_custom_event(vd, BleSpamCustomEventToggle);
            return true;
        }
        if(event->key == InputKeyUp) {
            view_dispatcher_send_custom_event(vd, BleSpamCustomEventSpeedUp);
            return true;
        }
        if(event->key == InputKeyDown) {
            view_dispatcher_send_custom_event(vd, BleSpamCustomEventSpeedDown);
            return true;
        }
    }
    return false;
}

View* ble_spam_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(BleSpamRunningModel));
    view_set_draw_callback(view, ble_spam_view_draw_callback);
    view_set_input_callback(view, ble_spam_view_input_callback);
    return view;
}

void ble_spam_view_free(View* view) {
    view_free(view);
}
