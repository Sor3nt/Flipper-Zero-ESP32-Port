#include "ble_walk_detail_view.h"
#include "../ble_spam_app.h"
#include <gui/canvas.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static void walk_detail_draw_callback(Canvas* canvas, void* _model) {
    BleWalkDetailModel* model = _model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, model->uuid_str);
    canvas_draw_line(canvas, 0, 12, 128, 12);

    canvas_set_font(canvas, FontSecondary);

    // Properties
    char props[32] = "Props:";
    if(model->properties & 0x02) strcat(props, " R");
    if(model->properties & 0x04) strcat(props, " WnR");
    if(model->properties & 0x08) strcat(props, " W");
    if(model->properties & 0x10) strcat(props, " N");
    if(model->properties & 0x20) strcat(props, " I");
    canvas_draw_str(canvas, 2, 24, props);

    // Value
    if(model->read_pending) {
        canvas_draw_str(canvas, 2, 36, "Reading...");
    } else if(model->value_len > 0) {
        char hex[64] = "";
        int pos = 0;
        for(int i = 0; i < model->value_len && pos < 58; i++) {
            pos += snprintf(&hex[pos], sizeof(hex) - pos, "%02X ", model->value[i]);
        }
        canvas_draw_str(canvas, 2, 36, hex);

        // Try to show as ASCII on second line
        char ascii[32] = "";
        int ap = 0;
        for(int i = 0; i < model->value_len && ap < 28; i++) {
            ascii[ap++] = (model->value[i] >= 0x20 && model->value[i] < 0x7F) ?
                          model->value[i] : '.';
        }
        ascii[ap] = '\0';
        canvas_draw_str(canvas, 2, 46, ascii);
    } else {
        canvas_draw_str(canvas, 2, 36, "(no data)");
    }

    canvas_set_font(canvas, FontPrimary);
    const char* hint = "";
    if((model->properties & 0x02) && (model->properties & 0x08)) {
        hint = "OK:Read  Down:Write";
    } else if(model->properties & 0x02) {
        hint = "OK: Read";
    } else if(model->properties & 0x08) {
        hint = "OK: Write";
    }
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, hint);
}

static bool walk_detail_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyOk || event->key == InputKeyDown) {
            view_dispatcher_send_custom_event(vd, event->key);
            return true;
        }
    }
    return false;
}

View* ble_walk_detail_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(BleWalkDetailModel));
    view_set_draw_callback(view, walk_detail_draw_callback);
    view_set_input_callback(view, walk_detail_input_callback);
    return view;
}

void ble_walk_detail_view_free(View* view) {
    view_free(view);
}
