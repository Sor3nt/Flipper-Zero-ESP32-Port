#include "wlan_portscan_view.h"
#include "wlan_view_common.h"
#include "wlan_view_events.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static void wlan_portscan_view_draw_callback(Canvas* canvas, void* _model) {
    WlanPortscanViewModel* model = _model;
    canvas_clear(canvas);

    wlan_view_draw_header(canvas, model->target_ip[0] ? model->target_ip : "Port Scan");

    if(model->count == 0) {
        wlan_view_draw_empty_box(
            canvas, model->scanning ? model->status : "No open ports");
        return;
    }

    canvas_set_font(canvas, FontSecondary);
    int line_height = 12;
    int header_height = WLAN_VIEW_HEADER_LINE_Y + 3;

    // Service-Spalte dynamisch nach Pixel-Breite des längsten Ports ausrichten,
    // damit alle Service-Labels bündig stehen.
    uint16_t max_port_w = 0;
    for(int i = 0; i < model->count; ++i) {
        char pbuf[8];
        snprintf(pbuf, sizeof(pbuf), "%d", model->ports[i].port);
        uint16_t w = canvas_string_width(canvas, pbuf);
        if(w > max_port_w) max_port_w = w;
    }
    uint8_t col2_x = 2 + max_port_w + 6; // 2 px Rand + max-port + 6 px Spacer

    for(int i = 0;
        i < WLAN_PORTSCAN_ITEMS_ON_SCREEN && (model->window_offset + i) < model->count;
        i++) {
        int idx = model->window_offset + i;
        WlanPortscanEntry* p = &model->ports[idx];
        int y = header_height + i * line_height;

        if(idx == model->selected) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y, 128, line_height);
            canvas_set_color(canvas, ColorWhite);
        }

        char port_buf[8];
        snprintf(port_buf, sizeof(port_buf), "%d", p->port);
        canvas_draw_str(canvas, 2, y + 10, port_buf);
        if(p->service[0]) {
            canvas_draw_str(canvas, col2_x, y + 10, p->service);
        }

        if(idx == model->selected) {
            canvas_set_color(canvas, ColorBlack);
        }
    }

    if(model->count > WLAN_PORTSCAN_ITEMS_ON_SCREEN) {
        elements_scrollbar(canvas, model->selected, model->count);
    }

    canvas_set_font(canvas, FontSecondary);
    if(model->scanning) {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, model->status);
    }
}

static bool wlan_portscan_view_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    if(event->key == InputKeyUp || event->key == InputKeyDown) {
        view_dispatcher_send_custom_event(vd, event->key);
        return true;
    }
    if(event->key == InputKeyOk && event->type == InputTypeShort) {
        view_dispatcher_send_custom_event(vd, WlanAppCustomEventPortscanOk);
        return true;
    }

    return false;
}

View* wlan_portscan_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(WlanPortscanViewModel));
    view_set_draw_callback(view, wlan_portscan_view_draw_callback);
    view_set_input_callback(view, wlan_portscan_view_input_callback);
    return view;
}

void wlan_portscan_view_free(View* view) {
    view_free(view);
}

uint8_t wlan_portscan_view_get_selected(View* view) {
    WlanPortscanViewModel* model = view_get_model(view);
    uint8_t s = model->selected;
    view_commit_model(view, false);
    return s;
}
