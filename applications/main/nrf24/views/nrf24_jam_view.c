#include "nrf24_jam_view.h"
#include "../helpers/nrf24_jam_config.h"

#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <assets_icons.h>
#include <stdio.h>

static void nrf24_jam_draw_band(Canvas* canvas, const Nrf24JamModel* model) {
    const int sx = 2, sy = 43, sw = 124, sh = 8;
    canvas_draw_frame(canvas, sx, sy, sw, sh);

    /* Active channels as short ticks from the baseline. */
    for(uint8_t i = 0; i < model->band_count; i++) {
        uint8_t c = model->channels[i];
        if(c > 124) continue;
        int x = sx + 1 + (c * (sw - 2)) / 125;
        canvas_draw_line(canvas, x, sy + sh - 2, x, sy + sh - 4);
    }

    /* Current channel: full-height marker. */
    if(model->running && model->channel <= 124) {
        int mx = sx + 1 + (model->channel * (sw - 2)) / 125;
        canvas_draw_line(canvas, mx, sy + 1, mx, sy + sh - 2);
    }
}

static void nrf24_jam_draw_callback(Canvas* canvas, void* _model) {
    Nrf24JamModel* model = _model;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    if(!model->hardware_ok) {
        canvas_draw_icon(canvas, 60, 18, &I_Quest_7x8);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, "NRF24 not found");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignCenter, "Hardware fault?");
        return;
    }

    /* ── Row 0: source type (left) + strategy badge (right, filled = running) ── */
    const char* strat = nrf24_jam_strategy_label(model->strategy);
    canvas_set_font(canvas, FontSecondary);
    int bw = canvas_string_width(canvas, strat) + 7;
    int bx = 127 - bw;
    if(model->running) {
        canvas_draw_box(canvas, bx, 0, bw, 11);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str_aligned(canvas, bx + bw / 2, 1, AlignCenter, AlignTop, strat);
        canvas_set_color(canvas, ColorBlack);
    } else {
        canvas_draw_rframe(canvas, bx, 0, bw, 11, 2);
        canvas_draw_str_aligned(canvas, bx + bw / 2, 1, AlignCenter, AlignTop, strat);
    }
    canvas_draw_str_aligned(canvas, 0, 1, AlignLeft, AlignTop, model->source_label);

    /* ── Row 1 (y=12): selection + hop-order flag ── */
    canvas_draw_str_aligned(canvas, 0, 12, AlignLeft, AlignTop, model->selection_label);
    if(model->random_hop) {
        canvas_draw_str_aligned(canvas, 127, 12, AlignRight, AlignTop, "RND");
    }

    /* ── Channel display (y=22, prominent, centered) ── */
    char line[40];
    canvas_set_font(canvas, FontPrimary);
    if(model->show_scan) {
        canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignTop, "Press Scan");
    } else if(!model->can_jam) {
        canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignTop, "no channels");
    } else {
        snprintf(line, sizeof(line), "CH %u   %u MHz", model->channel, 2400u + model->channel);
        canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignTop, line);
    }

    /* ── Timer · throughput · sweeps (y=34, centered) ── */
    canvas_set_font(canvas, FontSecondary);
    unsigned long secs = model->elapsed_ms / 1000UL;
    snprintf(
        line,
        sizeof(line),
        "%02lu:%02lu  %luc/s  S:%lu",
        secs / 60UL,
        secs % 60UL,
        (unsigned long)model->ch_per_sec,
        (unsigned long)model->sweeps);
    canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignTop, line);

    /* ── Band visualisation (y=43) ── */
    nrf24_jam_draw_band(canvas, model);

    /* ── Button hints: Left=Config (mapped to the Up/config event), OK=Run/Scan,
     * Right=Sel. ── */
    elements_button_left(canvas, "Config");
    elements_button_right(canvas, "Sel");
    elements_button_center(
        canvas, model->show_scan ? "Scan" : (model->running ? "Stop" : "Run"));
}

static bool nrf24_jam_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;

    switch(event->key) {
    case InputKeyOk:
        if(event->type == InputTypeShort) {
            view_dispatcher_send_custom_event(vd, Nrf24JamEventToggle);
            return true;
        }
        if(event->type == InputTypeLong) {
            view_dispatcher_send_custom_event(vd, Nrf24JamEventRescan);
            return true;
        }
        return false;
    case InputKeyUp:
        if(event->type != InputTypeShort) return false;
        view_dispatcher_send_custom_event(vd, Nrf24JamEventConfig);
        return true;
    case InputKeyLeft:
        /* Left button = Config (same event as Up). */
        if(event->type != InputTypeShort) return false;
        view_dispatcher_send_custom_event(vd, Nrf24JamEventConfig);
        return true;
    case InputKeyDown:
        if(event->type != InputTypeShort) return false;
        view_dispatcher_send_custom_event(vd, Nrf24JamEventCycleSource);
        return true;
    case InputKeyRight:
        if(event->type != InputTypeShort) return false;
        view_dispatcher_send_custom_event(vd, Nrf24JamEventSelectNext);
        return true;
    default:
        return false;
    }
}

View* nrf24_jam_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(Nrf24JamModel));
    view_set_draw_callback(view, nrf24_jam_draw_callback);
    view_set_input_callback(view, nrf24_jam_input_callback);
    return view;
}

void nrf24_jam_view_free(View* view) {
    view_free(view);
}
