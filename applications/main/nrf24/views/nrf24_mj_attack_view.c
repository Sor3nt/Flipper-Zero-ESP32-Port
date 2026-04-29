#include "nrf24_mj_attack_view.h"

#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <assets_icons.h>
#include <stdio.h>

static const char* phase_label(MjAttackPhase p) {
    switch(p) {
    case MjAttackPhaseScanning: return "Scanning";
    case MjAttackPhaseRunning: return "Running";
    case MjAttackPhaseDone: return "Done";
    case MjAttackPhaseError: return "Error";
    default: return "Idle";
    }
}

static void nrf24_mj_attack_draw_callback(Canvas* canvas, void* _model) {
    Nrf24MjAttackModel* model = _model;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    /* Header */
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "MouseJacker");
    canvas_draw_line(canvas, 0, 13, 127, 13);

    if(!model->hardware_ok) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_icon(canvas, 60, 22, &I_Quest_7x8);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "NRF24 not found");
        return;
    }

    canvas_set_font(canvas, FontSecondary);

    /* Phase + target */
    char hl[36];
    if(model->target_label[0]) {
        snprintf(hl, sizeof(hl), "%s  %s", phase_label(model->phase), model->target_label);
    } else if(model->phase == MjAttackPhaseScanning) {
        snprintf(hl, sizeof(hl), "Scanning ch %u", model->current_channel);
    } else {
        snprintf(hl, sizeof(hl), "%s", phase_label(model->phase));
    }
    canvas_draw_str_aligned(canvas, 64, 16, AlignCenter, AlignTop, hl);

    /* Script + progress */
    if(model->script_name[0]) {
        canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignTop, model->script_name);
    }

    if(model->phase == MjAttackPhaseRunning && model->line_total > 0) {
        const int bar_x = 8;
        const int bar_y = 38;
        const int bar_w = 112;
        const int bar_h = 7;
        canvas_draw_frame(canvas, bar_x, bar_y, bar_w, bar_h);
        int fill = (int)((bar_w - 2) * model->line_cur / model->line_total);
        if(fill > 0) canvas_draw_box(canvas, bar_x + 1, bar_y + 1, fill, bar_h - 2);

        char pb[24];
        snprintf(pb, sizeof(pb), "%u / %u", (unsigned)model->line_cur, (unsigned)model->line_total);
        canvas_draw_str_aligned(canvas, 64, 47, AlignCenter, AlignTop, pb);
    } else if(model->phase == MjAttackPhaseError && model->error_text[0]) {
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignTop, model->error_text);
    } else if(model->last_warning[0]) {
        canvas_draw_str_aligned(canvas, 64, 47, AlignCenter, AlignTop, model->last_warning);
    }

    /* Footer */
    if(model->phase == MjAttackPhaseRunning || model->phase == MjAttackPhaseScanning) {
        elements_button_center(canvas, "Stop");
    } else {
        elements_button_center(canvas, "Back");
    }
}

static bool nrf24_mj_attack_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type != InputTypeShort) return false;
    if(event->key == InputKeyOk) {
        view_dispatcher_send_custom_event(vd, Nrf24MjAttackEventStop);
        return true;
    }
    return false;
}

View* nrf24_mj_attack_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(Nrf24MjAttackModel));
    view_set_draw_callback(view, nrf24_mj_attack_draw_callback);
    view_set_input_callback(view, nrf24_mj_attack_input_callback);
    return view;
}

void nrf24_mj_attack_view_free(View* view) {
    view_free(view);
}
