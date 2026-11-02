#include "nrf24_spectrum_view.h"

#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <assets_icons.h>
#include <stdio.h>

#define BAR_TOP    13
#define BAR_BOTTOM 52
#define SCALE_Y    54

/* Map an NRF24 channel index (0..125) to an x-pixel on the 128 px canvas. */
#define BAR_X(ch) (((int)(ch) * 128) / NRF24_SPECTRUM_CHANNELS)

static const uint8_t scale_marks[] = {0, 25, 50, 75, 100, 125};

static void nrf24_spectrum_draw_callback(Canvas* canvas, void* _model) {
    Nrf24SpectrumModel* model = _model;
    canvas_clear(canvas);

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "NRF24 Spectrum");
    if(model->sample_errors) {
        char error_text[12];
        if(model->sample_errors > 99) {
            snprintf(error_text, sizeof(error_text), "E:99+");
        } else {
            snprintf(error_text, sizeof(error_text), "E:%lu", (unsigned long)model->sample_errors);
        }
        canvas_draw_str_aligned(canvas, 127, 0, AlignRight, AlignTop, error_text);
    }

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
        /* 1.5x boost so weak signals are still visible; clamp to bar height */
        int h = (level * bar_h * 3) / (125 * 2);
        if(h > bar_h) h = bar_h;
        if(h > 0) {
            int x = BAR_X(i);
            int w = BAR_X(i + 1) - x;
            if(w < 1) w = 1;
            canvas_draw_box(canvas, x, BAR_BOTTOM - h, w, h);
        }
    }

    /* Channel scale labels across the full nRF24 channel range. */
    canvas_set_font(canvas, FontSecondary);
    char tick[4];
    for(size_t i = 0; i < sizeof(scale_marks) / sizeof(scale_marks[0]); i++) {
        uint8_t ch = scale_marks[i];
        int x = BAR_X(ch);
        canvas_draw_dot(canvas, x, BAR_BOTTOM + 1);
        snprintf(tick, sizeof(tick), "%u", ch);
        const Align align = (i == 0) ? AlignLeft :
                            (i + 1 == sizeof(scale_marks) / sizeof(scale_marks[0])) ? AlignRight :
                                                                                     AlignCenter;
        canvas_draw_str_aligned(canvas, x, SCALE_Y + 1, align, AlignTop, tick);
    }
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
