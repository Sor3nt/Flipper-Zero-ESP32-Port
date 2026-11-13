#include "wlan_sae_flood_view.h"
#include "wlan_view_events.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <input/input.h>
#include <stdio.h>
#include <string.h>

#define SCREEN_W 128
#define BTN_Y    50
#define BTN_H    12

static void fmt_count(char* buf, size_t sz, unsigned long v) {
    if(v < 10000UL) {
        snprintf(buf, sz, "%lu", v);
    } else if(v < 10000000UL) {
        snprintf(buf, sz, "%luk", v / 1000UL);
    } else {
        snprintf(buf, sz, "%luM", v / 1000000UL);
    }
}

static void wlan_sae_flood_view_draw_callback(Canvas* canvas, void* context) {
    WlanSaeFloodModel* m = context;
    char line[64];

    canvas_clear(canvas);

    // Titel + "WPA3"-Marker rechts, Trennlinie.
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 2, 10, AlignLeft, AlignBottom, "SAE Flood");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, SCREEN_W - 2, 10, AlignRight, AlignBottom, "WPA3");
    canvas_draw_line(canvas, 0, 12, SCREEN_W - 1, 12);

    // Statuszeile (von der Scene gefüllt: READY/SCANNING/FLOOD n/NO WPA3/STOPPED).
    const char* status = (m->status[0] != '\0') ? m->status : "SYSTEM READY";
    canvas_draw_str_aligned(canvas, SCREEN_W / 2, 28, AlignCenter, AlignBottom, status);

    // Metrikzeile.
    if(m->running) {
        char pk[8];
        fmt_count(pk, sizeof(pk), (unsigned long)m->frames);
        snprintf(line, sizeof(line), "CH%u  %sp  %lus",
                 m->current_channel, pk, (unsigned long)(m->elapsed_ms / 1000U));
    } else {
        snprintf(line, sizeof(line), "SAE commit flood");
    }
    canvas_draw_str_aligned(canvas, SCREEN_W / 2, 40, AlignCenter, AlignBottom, line);

    // Start/Stop-Taste, mittig. Feste Breite, damit sie bei Start<->Stop nicht springt.
    const char* txt = m->running ? "Stop" : "Start";
    uint16_t tw = canvas_string_width(canvas, "Start");
    uint16_t sw = canvas_string_width(canvas, "Stop");
    if(sw > tw) tw = sw;
    int w = (int)tw + 12;
    int x = (SCREEN_W - w) / 2;
    elements_slightly_rounded_frame(canvas, x, BTN_Y, w, BTN_H);
    canvas_draw_str_aligned(canvas, x + w / 2, BTN_Y + BTN_H / 2 + 1, AlignCenter, AlignCenter, txt);
}

static bool wlan_sae_flood_view_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* dispatcher = context;
    if(event->type != InputTypeShort && event->type != InputTypeLong) return false;
    if(event->key == InputKeyOk) {
        view_dispatcher_send_custom_event(dispatcher, WlanAppCustomEventSaeFloodOk);
        return true;
    }
    return false; // Back etc. → Scene-Manager
}

View* wlan_sae_flood_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(WlanSaeFloodModel));
    view_set_draw_callback(view, wlan_sae_flood_view_draw_callback);
    view_set_input_callback(view, wlan_sae_flood_view_input_callback);
    return view;
}

void wlan_sae_flood_view_free(View* view) {
    view_free(view);
}
