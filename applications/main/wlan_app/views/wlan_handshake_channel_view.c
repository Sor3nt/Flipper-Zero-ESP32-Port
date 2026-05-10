#include "wlan_handshake_channel_view.h"
#include "wlan_view_events.h"
#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>
#include <string.h>

static void hsc_view_draw_callback(Canvas* canvas, void* _model) {
    WlanHsChannelViewModel* model = _model;
    canvas_clear(canvas);

    // Header: "Deauth:N" links (wenn aktiv), "Handshakes" zentriert, "Ch:N" rechts.
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 10, AlignCenter, AlignBottom, "Handshakes");
    if(model->deauth_active) {
        canvas_set_font(canvas, FontBatteryPercent);
        char dbuf[16];
        snprintf(dbuf, sizeof(dbuf), "%lu", (unsigned long)model->deauth_frames);
        canvas_draw_str(canvas, 2, 10, dbuf);
    }
    canvas_set_font(canvas, FontSecondary);
    char ch[10];
    snprintf(ch, sizeof(ch), "Ch:%u", (unsigned)model->channel);
    uint16_t cw = canvas_string_width(canvas, ch);
    canvas_draw_str(canvas, 128 - 3 - cw, 10, ch);
    canvas_draw_line(canvas, 0, 12, 128, 12);

    // Body: drei zentrierte Zeilen.
    canvas_set_font(canvas, FontSecondary);
    char buf[80];
    snprintf(buf, sizeof(buf), "Observing %u Wifi", (unsigned)model->count);
    canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignBottom, buf);

    {
        // "Found:" alterniert alle 2 s mit dem "Hold OK to Deauth"-Hint.
        // Sobald Deauth aktiv ist (Hold-OK oder Auto), bleibt "Found:" fest.
        // Bei Hopping ist Deauth ohnehin gesperrt → kein Hint nötig.
        bool show_hold_hint =
            !model->deauth_active && !model->hopping &&
            ((furi_get_tick() / 2000) % 2 == 1);
        if(show_hold_hint) {
            canvas_set_font(canvas, FontPrimary);
            uint16_t w1 = canvas_string_width(canvas, "Hold OK");
            canvas_set_font(canvas, FontSecondary);
            uint16_t w2 = canvas_string_width(canvas, " to Deauth");
            int16_t x_start = (int16_t)((128 - (int16_t)(w1 + w2)) / 2);
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str(canvas, x_start, 34, "Hold OK");
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, x_start + w1, 34, " to Deauth");
        } else {
            const char* src =
                model->latest_handshake_ssid[0] ? model->latest_handshake_ssid : "none";
            snprintf(buf, sizeof(buf), "Found: %s", src);
            FuriString* s = furi_string_alloc_set(buf);
            elements_string_fit_width(canvas, s, 124);
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignBottom, furi_string_get_cstr(s));
            furi_string_free(s);
        }
    }

    canvas_set_font(canvas, FontPrimary);
    snprintf(buf, sizeof(buf), "Handshake Captured: %u", (unsigned)model->hs_complete_count);
    canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignBottom, buf);

    // Trennlinie unten + Soft-Buttons.
    canvas_draw_line(canvas, 0, 51, 128, 51);
    elements_button_left(canvas, "Config");
    // Auto-Deauth nur sinnvoll bei festem Channel — bei Hopping wechselt
    // der Channel mitten im Deauth-Stream.
    if(!model->hopping) {
        elements_button_right(canvas, model->auto_mode ? "Stop" : "Auto");
    }
}

static bool hsc_view_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;

    // OK Press/Release: Hold-Deauth (unverändert).
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

    // Up: Settings öffnen (Soft-Button-Left).
    if(event->type == InputTypeShort && event->key == InputKeyUp) {
        view_dispatcher_send_custom_event(vd, WlanAppCustomEventHandshakeSettingsOpen);
        return true;
    }

    // Down: Auto-Deauth toggeln (Soft-Button-Right).
    if(event->type == InputTypeShort && event->key == InputKeyDown) {
        view_dispatcher_send_custom_event(vd, WlanAppCustomEventHandshakeAutoToggle);
        return true;
    }

    return false;
}

View* wlan_handshake_channel_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(WlanHsChannelViewModel));
    view_set_draw_callback(view, hsc_view_draw_callback);
    view_set_input_callback(view, hsc_view_input_callback);
    return view;
}

void wlan_handshake_channel_view_free(View* view) {
    view_free(view);
}
