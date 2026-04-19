#include "ble_walk_scan_view.h"
#include "../ble_spam_app.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static void walk_scan_draw_callback(Canvas* canvas, void* _model) {
    BleWalkScanModel* model = _model;
    canvas_clear(canvas);

    // Draw device list
    canvas_set_font(canvas, FontPrimary);
    char title[32];
    snprintf(title, sizeof(title), "BLE Scan (%d)", model->count);
    canvas_draw_str(canvas, 2, 10, title);
    canvas_draw_line(canvas, 0, 12, 128, 12);

    canvas_set_font(canvas, FontSecondary);

    if(model->count == 0 && model->scanning) {
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "Scanning...");
    }

    for(int i = 0; i < WALK_SCAN_ITEMS_ON_SCREEN && (model->window_offset + i) < model->count; i++) {
        uint16_t idx = model->window_offset + i;
        BleWalkDevice* dev = &model->devices[idx];

        char line[40];
        if(dev->name[0]) {
            snprintf(line, sizeof(line), "%d %.18s", dev->rssi, dev->name);
        } else {
            snprintf(line, sizeof(line), "%d %02X:%02X:%02X:%02X:%02X:%02X",
                     dev->rssi,
                     dev->addr[0], dev->addr[1], dev->addr[2],
                     dev->addr[3], dev->addr[4], dev->addr[5]);
        }

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
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK:Connect");

    // Draw centered status overlay
    if(model->connect_status != WalkScanStatusNone) {
        const char* msg = "";
        switch(model->connect_status) {
        case WalkScanStatusConnecting: msg = "Connecting..."; break;
        case WalkScanStatusConnected:  msg = "Connected!"; break;
        case WalkScanStatusFailed:     msg = "Failed!"; break;
        default: break;
        }

        uint16_t w = canvas_string_width(canvas, msg) + 12;
        if(w < 80) w = 80;
        uint16_t x = (128 - w) / 2;
        uint16_t y = 20;

        // Clear background
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, x, y, w, 22);
        canvas_set_color(canvas, ColorBlack);
        elements_frame(canvas, x, y, w, 22);
        canvas_draw_str_aligned(canvas, 64, y + 12, AlignCenter, AlignCenter, msg);
    }
}

static bool walk_scan_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyOk || event->key == InputKeyUp || event->key == InputKeyDown) {
            view_dispatcher_send_custom_event(vd, event->key);
            return true;
        }
    }
    return false;
}

View* ble_walk_scan_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(BleWalkScanModel));
    view_set_draw_callback(view, walk_scan_draw_callback);
    view_set_input_callback(view, walk_scan_input_callback);
    return view;
}

void ble_walk_scan_view_free(View* view) {
    view_free(view);
}
