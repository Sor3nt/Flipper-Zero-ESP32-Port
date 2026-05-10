#include "wlan_handshake_view.h"
#include "wlan_view_common.h"
#include "wlan_view_events.h"
#include <gui/canvas.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>
#include <string.h>

static void wlan_handshake_view_draw_callback(Canvas* canvas, void* _model) {
    WlanHandshakeViewModel* model = _model;
    canvas_clear(canvas);

    const char* header_text;
    if(model->title[0]) {
        header_text = model->title;
    } else if(model->ssid[0]) {
        header_text = model->ssid;
    } else {
        header_text = "(no target)";
    }
    wlan_view_draw_header(canvas, header_text);

    // Deauth-Counter (klein) links im Header wenn aktiv.
    if(model->deauth_active) {
        canvas_set_font(canvas, FontBatteryPercent);
        char dbuf[16];
        snprintf(dbuf, sizeof(dbuf), "%lu", (unsigned long)model->deauth_frames);
        canvas_draw_str(canvas, 2, 10, dbuf);
    }

    // M1..M4 + B Boxen.
    canvas_set_font(canvas, FontSecondary);
    const char* labels[] = {"M1", "M2", "M3", "M4", "B"};
    const bool* flags[] = {
        &model->has_m1, &model->has_m2, &model->has_m3,
        &model->has_m4, &model->has_beacon};
    for(int i = 0; i < 5; i++) {
        int x = 2 + i * 25;
        int y = 15;
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

    // Clock-Symbol + Time (zentriert).
    {
        char t[16];
        uint32_t mn = model->elapsed_sec / 60;
        uint32_t sc = model->elapsed_sec % 60;
        snprintf(t, sizeof(t), "%02lu:%02lu", (unsigned long)mn, (unsigned long)sc);
        const uint8_t clock_w = 8;
        const uint8_t spacer = 3;
        uint16_t tw = canvas_string_width(canvas, t);
        int total = clock_w + spacer + tw;
        int sx = (128 - total) / 2;
        int cy = 33;
        int cx = sx + 4;
        canvas_draw_circle(canvas, cx, cy, 3);
        canvas_draw_dot(canvas, cx, cy);
        canvas_draw_line(canvas, cx, cy, cx, cy - 2);    // Zeiger oben
        canvas_draw_line(canvas, cx, cy, cx + 2, cy);    // Zeiger rechts
        canvas_draw_str(canvas, sx + clock_w + spacer, cy + 4, t);
    }

    // Deauth-Hint / aktive Deauth (zentriert).
    {
        char d[24];
        const char* line = d;
        if(model->deauth_active) {
            snprintf(d, sizeof(d), "Deauth: %lu", (unsigned long)model->deauth_frames);
        } else {
            line = "Hold OK for Deauth";
        }
        canvas_draw_str_aligned(canvas, 64, 47, AlignCenter, AlignBottom, line);
    }

    // Status (FontPrimary, zentriert).
    canvas_set_font(canvas, FontPrimary);
    const char* status;
    if(model->complete) {
        status = "Handshake saved!";
    } else if(model->running) {
        status = model->deauth_active ? "Capturing + Deauth" : "Capturing";
    } else {
        status = "Stopped";
    }
    canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, status);
}

static bool wlan_handshake_view_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->key == InputKeyOk) {
        if(event->type == InputTypePress) {
            view_dispatcher_send_custom_event(vd, WlanAppCustomEventHandshakeDeauthStart);
            return true;
        }
        if(event->type == InputTypeRelease) {
            view_dispatcher_send_custom_event(vd, WlanAppCustomEventHandshakeDeauthStop);
            return true;
        }
    }
    // Up/Down: nur im Channel-Mode relevant. Die Scene mappt sie auf Channel-Up/Down,
    // wenn app->channel_mode_active gesetzt ist.
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyUp) {
            view_dispatcher_send_custom_event(vd, WlanAppCustomEventHandshakeChannelUp);
            return true;
        }
        if(event->key == InputKeyDown) {
            view_dispatcher_send_custom_event(vd, WlanAppCustomEventHandshakeChannelDown);
            return true;
        }
    }
    return false;
}

View* wlan_handshake_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(WlanHandshakeViewModel));
    view_set_draw_callback(view, wlan_handshake_view_draw_callback);
    view_set_input_callback(view, wlan_handshake_view_input_callback);
    return view;
}

void wlan_handshake_view_free(View* view) {
    view_free(view);
}
