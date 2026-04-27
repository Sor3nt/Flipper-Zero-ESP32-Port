#include "nrf24_spectrum_view.h"

#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <assets_icons.h>
#include <stdio.h>

#define BAR_TOP    14
#define BAR_BOTTOM 47
#define SCALE_Y    49

/* Map an NRF24 channel index (0..79) to an x-pixel on the 128 px wide canvas */
#define BAR_X(ch) (((int)(ch) * 128) / NRF24_SPECTRUM_CHANNELS)

static const uint8_t scale_marks[] = {0, 10, 20, 30, 40, 50, 60, 70};

static void nrf24_spectrum_draw_callback(Canvas* canvas, void* _model) {
    Nrf24SpectrumModel* model = _model;
    canvas_clear(canvas);

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "NRF24 Spectrum");

    if(!model->hardware_ok) {
        canvas_draw_icon(canvas, 60, 22, &I_Quest_7x8);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter, "NRF24 not found");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignCenter, "Hardware fault?");
        return;
    }

    /* Bargraph baseline */
    const int bar_h = BAR_BOTTOM - BAR_TOP;
    canvas_draw_line(canvas, 0, BAR_BOTTOM, 127, BAR_BOTTOM);

    for(int i = 0; i < NRF24_SPECTRUM_CHANNELS; i++) {
        int level = model->levels[i];
        int h = (level * bar_h) / 125;
        if(h > bar_h) h = bar_h;
        if(h > 0) {
            int x = BAR_X(i);
            int w = BAR_X(i + 1) - x;
            if(w < 1) w = 1;
            canvas_draw_box(canvas, x, BAR_BOTTOM - h, w, h);
        }
    }

    /* Channel scale labels — every 10 channels */
    canvas_set_font(canvas, FontSecondary);
    char tick[4];
    for(size_t i = 0; i < sizeof(scale_marks) / sizeof(scale_marks[0]); i++) {
        uint8_t ch = scale_marks[i];
        int x = BAR_X(ch) + (BAR_X(ch + 1) - BAR_X(ch)) / 2;
        canvas_draw_dot(canvas, x, BAR_BOTTOM + 1);
        snprintf(tick, sizeof(tick), "%u", ch);
        canvas_draw_str_aligned(canvas, x, SCALE_Y + 1, AlignCenter, AlignTop, tick);
    }

    /* Footer status */
    char buf[24];
    snprintf(buf, sizeof(buf), "%lu sweeps", (unsigned long)model->sweep_count);
    canvas_draw_str(canvas, 0, 63, buf);
    canvas_draw_str_aligned(canvas, 127, 63, AlignRight, AlignBottom, "2.40-2.48GHz");
}

static bool nrf24_spectrum_input_callback(InputEvent* event, void* context) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

View* nrf24_spectrum_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(Nrf24SpectrumModel));
    view_set_draw_callback(view, nrf24_spectrum_draw_callback);
    view_set_input_callback(view, nrf24_spectrum_input_callback);
    return view;
}

void nrf24_spectrum_view_free(View* view) {
    view_free(view);
}
