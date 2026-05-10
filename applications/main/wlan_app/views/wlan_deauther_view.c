#include "wlan_deauther_view.h"
#include "wlan_view_common.h"
#include "wlan_view_events.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static void wlan_deauther_view_draw_callback(Canvas* canvas, void* _model) {
    WlanDeautherModel* model = _model;
    canvas_clear(canvas);

    wlan_view_draw_header(canvas, model->title[0] ? model->title : "Deauth");

    // "SSID" Label (FontPrimary, fett) + WiFi-Name (FontSecondary).
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignBottom, "SSID");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignBottom,
        model->ssid[0] ? model->ssid : "(no target)");

    // Mittlere Zeile: Scanning > Status > Live-Counter > Hold-OK-Hint (in dieser Priorität).
    if(model->scanning) {
        char buf[40];
        snprintf(buf, sizeof(buf), "Scanning ch %u...", (unsigned)model->channel);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 47, AlignCenter, AlignBottom, buf);
    } else if(model->status_msg[0]) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 47, AlignCenter, AlignBottom, model->status_msg);
    } else if(model->running) {
        char buf[40];
        snprintf(buf, sizeof(buf), "Deauth: %lu", (unsigned long)model->frames_sent);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 47, AlignCenter, AlignBottom, buf);
    } else {
        canvas_set_font(canvas, FontPrimary);
        uint16_t w1 = canvas_string_width(canvas, "Hold OK");
        canvas_set_font(canvas, FontSecondary);
        uint16_t w2 = canvas_string_width(canvas, " to Deauth");
        int16_t x_start = (int16_t)((128 - (int16_t)(w1 + w2)) / 2);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, x_start, 47, "Hold OK");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, x_start + w1, 47, " to Deauth");
    }

    // Soft-Buttons unten links/rechts. Im Channel-Mode kein Targets-Button —
    // der Picker hätte dort keine Wirkung (Worker macht ausschließlich
    // Channel-Broadcast über alle gefundenen APs).
    if(model->channel_mode) {
        elements_button_left(canvas, "Ch-");
        elements_button_right(canvas, "Ch+");
        char chbuf[16];
        snprintf(chbuf, sizeof(chbuf), "Ch:%u", (unsigned)model->channel);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 61, AlignCenter, AlignBottom, chbuf);
    } else {
        elements_button_left(canvas, "Targets");
        elements_button_right(canvas, model->auto_mode ? "Stop" : "Auto");
        wlan_view_draw_target_counter(canvas, model->target_count);
    }
}

static bool wlan_deauther_view_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->key == InputKeyOk) {
        if(event->type == InputTypePress) {
            view_dispatcher_send_custom_event(vd, WlanAppCustomEventDeautherStart);
            return true;
        }
        if(event->type == InputTypeRelease) {
            view_dispatcher_send_custom_event(vd, WlanAppCustomEventDeautherStop);
            return true;
        }
    }
    // Encoder Up/Down → Targets/Auto. Im Channel-Mode interpretiert die Scene
    // diese Events stattdessen als Channel-Up/Down (siehe scene_network_deauth.c).
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyUp) {
            view_dispatcher_send_custom_event(vd, WlanAppCustomEventDeautherTargets);
            return true;
        }
        if(event->key == InputKeyDown) {
            view_dispatcher_send_custom_event(vd, WlanAppCustomEventDeautherAuto);
            return true;
        }
    }
    return false;
}

View* wlan_deauther_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(WlanDeautherModel));
    view_set_draw_callback(view, wlan_deauther_view_draw_callback);
    view_set_input_callback(view, wlan_deauther_view_input_callback);
    return view;
}

void wlan_deauther_view_free(View* view) {
    view_free(view);
}
