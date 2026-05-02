#include "race_detector_view.h"
#include "../ble_spam_app.h"
#include <gui/canvas.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static char race_status_glyph(RaceStatus s) {
    switch(s) {
    case RaceStatusVulnerable:  return 'V';
    case RaceStatusClean:       return '-';
    case RaceStatusProbing:     return '.';
    case RaceStatusConnectFail: return 'x';
    case RaceStatusUnknown:
    default:                    return '?';
    }
}

static void race_detector_draw_callback(Canvas* canvas, void* _model) {
    RaceDetectorModel* model = _model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    char title[32];
    snprintf(title, sizeof(title), "RACE Detect (%d)", model->count);
    canvas_draw_str(canvas, 2, 10, title);
    canvas_draw_line(canvas, 0, 12, 128, 12);

    canvas_set_font(canvas, FontSecondary);

    if(model->count == 0) {
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "Scanning...");
    }

    for(int i = 0;
        i < RACE_DETECTOR_ITEMS_ON_SCREEN && (model->window_offset + i) < model->count;
        i++) {
        uint16_t idx = model->window_offset + i;
        RaceDevice* d = &model->devices[idx];

        const char* label = d->name[0] ? d->name : "(unnamed)";

        char line[40];
        snprintf(
            line,
            sizeof(line),
            "%c %-11.11s %02X:%02X %d",
            race_status_glyph(d->status),
            label,
            d->addr[4],
            d->addr[5],
            d->rssi);

        uint8_t y = 22 + i * 11;
        if(idx == model->selected) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y - 8, 128, 11);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, 2, y, line);
        if(idx == model->selected) {
            canvas_set_color(canvas, ColorBlack);
        }
    }

    canvas_set_font(canvas, FontPrimary);
    if(model->probing) {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "Probing...");
    } else {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK:Probe");
    }
}

static bool race_detector_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyOk || event->key == InputKeyUp || event->key == InputKeyDown) {
            view_dispatcher_send_custom_event(vd, event->key);
            return true;
        }
    }
    return false;
}

View* race_detector_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(RaceDetectorModel));
    view_set_draw_callback(view, race_detector_draw_callback);
    view_set_input_callback(view, race_detector_input_callback);
    return view;
}

void race_detector_view_free(View* view) {
    view_free(view);
}
