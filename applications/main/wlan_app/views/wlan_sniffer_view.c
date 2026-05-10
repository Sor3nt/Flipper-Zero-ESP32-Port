#include "wlan_sniffer_view.h"
#include "wlan_view_common.h"
#include <furi.h>
#include <gui/elements.h>
#include <assets_icons.h>
#include <input/input.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    char title[24];
    uint32_t received;
    uint32_t elapsed_sec;
    uint8_t target_count;
    uint8_t channel;
    bool running;
    bool channel_mode;
} WlanSnifferViewModel;

struct WlanSnifferView {
    View* view;
    WlanSnifferViewActionCb action_cb;
    void* action_ctx;
};

static void format_count(uint32_t n, char* buf, size_t sz) {
    if(n < 1000) {
        snprintf(buf, sz, "%lu", (unsigned long)n);
    } else if(n < 1000000) {
        if(n >= 100000) {
            snprintf(buf, sz, "%luk", (unsigned long)(n / 1000));
        } else {
            snprintf(buf, sz, "%lu.%luk",
                (unsigned long)(n / 1000),
                (unsigned long)((n % 1000) / 100));
        }
    } else if(n < 1000000000) {
        if(n >= 100000000) {
            snprintf(buf, sz, "%luM", (unsigned long)(n / 1000000));
        } else {
            snprintf(buf, sz, "%lu.%luM",
                (unsigned long)(n / 1000000),
                (unsigned long)((n % 1000000) / 100000));
        }
    } else {
        snprintf(buf, sz, "%luG", (unsigned long)(n / 1000000000));
    }
}

static void wlan_sniffer_view_draw(Canvas* canvas, void* model) {
    WlanSnifferViewModel* m = model;
    canvas_clear(canvas);

    // Header.
    wlan_view_draw_header(canvas, m->title[0] ? m->title : "Sniffer");

    // Im Channel-Mode: aktuellen Channel rechts oben im Header anzeigen.
    if(m->channel_mode) {
        canvas_set_font(canvas, FontSecondary);
        char ch_buf[10];
        snprintf(ch_buf, sizeof(ch_buf), "Ch:%u", (unsigned)m->channel);
        uint16_t cw = canvas_string_width(canvas, ch_buf);
        canvas_draw_str(canvas, 128 - 3 - cw, WLAN_VIEW_HEADER_BASELINE_Y, ch_buf);
    }

    // Dolphin links — bleibt statisch (auch während Sniffing).
    canvas_draw_icon(canvas, 0, 14, &I_WarningDolphin_45x42);

    // Rechte Spalte: zentriert auf x=86 (Mitte zwischen Dolphin-Rand und Display-Ende).
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 86, 22, AlignCenter, AlignBottom, "Received");

    char buf[24];
    format_count(m->received, buf, sizeof(buf));
    canvas_draw_str_aligned(canvas, 86, 38, AlignCenter, AlignBottom, buf);

    canvas_set_font(canvas, FontSecondary);
    uint32_t mins = m->elapsed_sec / 60;
    uint32_t secs = m->elapsed_sec % 60;
    snprintf(buf, sizeof(buf), "Time: %lu:%02lu",
        (unsigned long)mins, (unsigned long)secs);
    canvas_draw_str_aligned(canvas, 86, 50, AlignCenter, AlignBottom, buf);

    // Soft-Buttons unten.
    if(m->channel_mode) {
        // Channel-Mode: Sniff/Stop in der Mitte (mit OK), kein Targets-Button.
        elements_button_center(canvas, m->running ? "Stop" : "Sniff");
    } else {
        elements_button_left(canvas, "Targets");
        elements_button_right(canvas, m->running ? "Stop" : "Sniff");
        wlan_view_draw_target_counter(canvas, m->target_count);
    }
}

static bool wlan_sniffer_view_input(InputEvent* event, void* context) {
    WlanSnifferView* v = context;
    if(event->type != InputTypeShort) return false;
    if(!v->action_cb) return false;

    bool channel_mode = false;
    with_view_model(v->view, WlanSnifferViewModel * m, { channel_mode = m->channel_mode; }, false);

    switch(event->key) {
    case InputKeyOk:
        v->action_cb(WlanSnifferViewActionToggle, v->action_ctx);
        return true;
    case InputKeyDown:
        v->action_cb(
            channel_mode ? WlanSnifferViewActionChannelDown : WlanSnifferViewActionToggle,
            v->action_ctx);
        return true;
    case InputKeyUp:
        v->action_cb(
            channel_mode ? WlanSnifferViewActionChannelUp : WlanSnifferViewActionTargets,
            v->action_ctx);
        return true;
    default:
        return false;
    }
}

WlanSnifferView* wlan_sniffer_view_alloc(void) {
    WlanSnifferView* v = malloc(sizeof(WlanSnifferView));
    v->view = view_alloc();
    v->action_cb = NULL;
    v->action_ctx = NULL;
    view_set_context(v->view, v);
    view_allocate_model(v->view, ViewModelTypeLockFree, sizeof(WlanSnifferViewModel));
    view_set_draw_callback(v->view, wlan_sniffer_view_draw);
    view_set_input_callback(v->view, wlan_sniffer_view_input);

    WlanSnifferViewModel* m = view_get_model(v->view);
    memset(m, 0, sizeof(*m));
    view_commit_model(v->view, false);

    return v;
}

void wlan_sniffer_view_free(WlanSnifferView* v) {
    furi_assert(v);
    view_free(v->view);
    free(v);
}

View* wlan_sniffer_view_get_view(WlanSnifferView* v) {
    return v->view;
}

void wlan_sniffer_view_set_title(WlanSnifferView* v, const char* title) {
    with_view_model(v->view, WlanSnifferViewModel * m, {
        strncpy(m->title, title ? title : "", sizeof(m->title) - 1);
        m->title[sizeof(m->title) - 1] = 0;
    }, true);
}

void wlan_sniffer_view_set_target_count(WlanSnifferView* v, uint8_t count) {
    with_view_model(v->view, WlanSnifferViewModel * m, { m->target_count = count; }, true);
}

void wlan_sniffer_view_set_channel_mode(WlanSnifferView* v, bool channel_mode) {
    with_view_model(v->view, WlanSnifferViewModel * m, { m->channel_mode = channel_mode; }, true);
}

void wlan_sniffer_view_set_channel(WlanSnifferView* v, uint8_t channel) {
    with_view_model(v->view, WlanSnifferViewModel * m, { m->channel = channel; }, true);
}

void wlan_sniffer_view_set_received(WlanSnifferView* v, uint32_t count) {
    with_view_model(v->view, WlanSnifferViewModel * m, { m->received = count; }, true);
}

void wlan_sniffer_view_set_elapsed(WlanSnifferView* v, uint32_t sec) {
    with_view_model(v->view, WlanSnifferViewModel * m, { m->elapsed_sec = sec; }, true);
}

void wlan_sniffer_view_set_running(WlanSnifferView* v, bool running) {
    with_view_model(v->view, WlanSnifferViewModel * m, { m->running = running; }, true);
}

void wlan_sniffer_view_reset_counters(WlanSnifferView* v) {
    with_view_model(v->view, WlanSnifferViewModel * m, {
        m->received = 0;
        m->elapsed_sec = 0;
    }, true);
}

void wlan_sniffer_view_set_action_callback(
    WlanSnifferView* v, WlanSnifferViewActionCb cb, void* ctx) {
    v->action_cb = cb;
    v->action_ctx = ctx;
}
