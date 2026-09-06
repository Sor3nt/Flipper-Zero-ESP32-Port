#include "wlan_smart_deauth_view.h"
#include "wlan_view_common.h"
#include "wlan_view_events.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static void wlan_smart_deauth_view_draw_callback(Canvas* canvas, void* _model) {
    WlanSmartDeauthModel* model = _model;
    canvas_clear(canvas);

    wlan_view_draw_header(canvas, "Smart Deauth");

    // Hauptwert: der (manuell wählbare) Channel.
    canvas_set_font(canvas, FontPrimary);
    char cbuf[20];
    snprintf(cbuf, sizeof(cbuf), "Channel %u", (unsigned)model->channel);
    canvas_draw_str_aligned(canvas, 64, 27, AlignCenter, AlignBottom, cbuf);

    // Status + Frame-Counter.
    canvas_set_font(canvas, FontSecondary);
    char buf[48];
    snprintf(
        buf, sizeof(buf), "%s   Deauth: %lu",
        model->running ? "RUNNING" : "Stopped", (unsigned long)model->frames);
    canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignBottom, buf);

    snprintf(buf, sizeof(buf), "APs on channel: %u", (unsigned)model->target_count);
    canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignBottom, buf);

    // Soft-Buttons unten (Optik/Bezeichnung wie wlan_deauther_view): Ch- / Ch+,
    // dazu ein zentraler Start/Stop-Button auf OK.
    elements_button_left(canvas, "Ch-");
    elements_button_center(canvas, model->running ? "Stop" : "Start");
    elements_button_right(canvas, "Ch+");
}

static bool wlan_smart_deauth_view_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;

    if(event->type == InputTypeShort) {
        // OK → Start/Stop toggeln.
        if(event->key == InputKeyOk) {
            view_dispatcher_send_custom_event(vd, WlanAppCustomEventSmartDeauthToggle);
            return true;
        }
        // Encoder rotate-left = Up → Channel−1 ; rotate-right = Down → Channel+1.
        if(event->key == InputKeyUp) {
            view_dispatcher_send_custom_event(vd, WlanAppCustomEventSmartDeauthChannelDown);
            return true;
        }
        if(event->key == InputKeyDown) {
            view_dispatcher_send_custom_event(vd, WlanAppCustomEventSmartDeauthChannelUp);
            return true;
        }
    }
    return false;
}

View* wlan_smart_deauth_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(WlanSmartDeauthModel));
    view_set_draw_callback(view, wlan_smart_deauth_view_draw_callback);
    view_set_input_callback(view, wlan_smart_deauth_view_input_callback);
    return view;
}

void wlan_smart_deauth_view_free(View* view) {
    view_free(view);
}
