#include "subghz_frequency_analyzer.h"

#include <furi.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <gui/elements.h>
#include "../helpers/subghz_frequency_analyzer_worker.h"

#include <assets_icons.h>
#include <float_tools.h>
#include <u8g2.h>

#define TAG "frequency_analyzer"

#define RSSI_MIN     (-97.0f)
#define RSSI_MAX     (-60.0f)
#define RSSI_SCALE   2.3f
#define TRIGGER_STEP 1
#define MAX_HISTORY  4
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#endif

typedef enum {
    SubGhzFrequencyAnalyzerStatusIDLE,
} SubGhzFrequencyAnalyzerStatus;

struct SubGhzFrequencyAnalyzer {
    View* view;
    SubGhzFrequencyAnalyzerWorker* worker;
    SubGhzFrequencyAnalyzerCallback callback;
    void* context;
    SubGhzTxRx* txrx;
    bool locked;
    SubGHzFrequencyAnalyzerFeedbackLevel
        feedback_level; // 0 - no feedback, 1 - vibro only, 2 - vibro and sound
    float rssi_last;
    uint8_t selected_index;
    uint8_t max_index;
    bool show_frame;
};

typedef struct {
    uint32_t frequency;
    uint32_t frequency_to_save;
    uint32_t history_active_frequency;
    float rssi;
    uint32_t history_frequency[MAX_HISTORY];
    uint8_t history_frequency_rx_count[MAX_HISTORY];
    bool signal;
    float rssi_last;
    float trigger;
    SubGHzFrequencyAnalyzerFeedbackLevel feedback_level;
    uint8_t selected_index;
    uint8_t max_index;
    bool show_frame;
    bool is_ext_radio;
} SubGhzFrequencyAnalyzerModel;

void subghz_frequency_analyzer_set_callback(
    SubGhzFrequencyAnalyzer* subghz_frequency_analyzer,
    SubGhzFrequencyAnalyzerCallback callback,
    void* context) {
    furi_assert(subghz_frequency_analyzer);
    furi_assert(callback);
    subghz_frequency_analyzer->callback = callback;
    subghz_frequency_analyzer->context = context;
}

void subghz_frequency_analyzer_draw_rssi(
    Canvas* canvas,
    float rssi,
    float rssi_last,
    float trigger,
    uint8_t x,
    uint8_t y) {
    // Current RSSI
    if(!float_is_equal(rssi, 0.f)) {
        if(rssi < RSSI_MIN) rssi = RSSI_MIN;
        if(rssi > RSSI_MAX) {
            rssi = RSSI_MAX;
        }
        rssi = (rssi - RSSI_MIN) / RSSI_SCALE;
        uint8_t column_number = 0;
        for(size_t i = 0; i <= (uint8_t)rssi; i++) {
            if((i + 1) % 4) {
                column_number++;
                // Reserve y=28..50 for the two history rows and their frames.
                uint8_t height = (column_number + 1) / 2;
                canvas_draw_box(canvas, x + 2 * i, (y + 4) - height, 2, height);
            }
        }
    }

    // Last RSSI
    if(!float_is_equal(rssi_last, 0.f)) {
        if(rssi_last < RSSI_MIN) rssi_last = RSSI_MIN;
        if(rssi_last > RSSI_MAX) {
            rssi_last = RSSI_MAX;
        }
        int max_x = (int)((rssi_last - RSSI_MIN) / RSSI_SCALE) * 2;
        //if(!(max_x % 8)) max_x -= 2;
        int max_h = (int)((rssi_last - RSSI_MIN) / RSSI_SCALE) + 1;
        max_h -= (max_h / 4) + 3;
        if(max_h < 0) max_h = 0;
        max_h = (max_h + 1) / 2;
        canvas_draw_line(canvas, x + max_x + 1, y - max_h, x + max_x + 1, y + 3);
    }

    // Trigger cursor
    if(trigger < RSSI_MIN) trigger = RSSI_MIN;
    if(trigger > RSSI_MAX) trigger = RSSI_MAX;
    trigger = (trigger - RSSI_MIN) / RSSI_SCALE;
    uint8_t tr_x = (uint8_t)((float)x + (2 * trigger));
    canvas_draw_dot(canvas, tr_x, y + 4);
    canvas_draw_line(canvas, tr_x - 1, y + 5, tr_x + 1, y + 5);

    canvas_draw_line(canvas, x, y + 3, x + (RSSI_MAX - RSSI_MIN) * 2 / RSSI_SCALE, y + 3);
}

static void subghz_frequency_analyzer_history_frequency_draw(
    Canvas* canvas,
    SubGhzFrequencyAnalyzerModel* model) {
    char buffer[64] = {0};
    const uint8_t x1 = 2;
    const uint8_t x2 = 66;
    const uint8_t y = 37;

    canvas_set_font(canvas, FontSecondary);
    uint8_t line = 0;
    bool show_frame = model->show_frame && model->max_index > 0;
    for(uint8_t i = 0; i < MAX_HISTORY; i++) {
        uint8_t current_x;
        uint8_t current_y = y + line * 11;

        if(i % 2 == 0) {
            current_x = x1;
        } else {
            current_x = x2;
            line++;
        }
        if(model->history_frequency[i]) {
            snprintf(
                buffer,
                sizeof(buffer),
                "%03lu.%03lu",
                (unsigned long)(model->history_frequency[i] / 1000000 % 1000),
                (unsigned long)(model->history_frequency[i] / 1000 % 1000));
            canvas_draw_str(canvas, current_x, current_y, buffer);
        } else {
            canvas_draw_str(canvas, current_x, current_y, "---.---");
        }
        if(model->history_frequency_rx_count[i] > 0) {
            snprintf(buffer, sizeof(buffer), "x%d", model->history_frequency_rx_count[i]);
            canvas_draw_str(canvas, current_x + 41, current_y, buffer);
        } else {
            canvas_draw_str(canvas, current_x + 41, current_y, "MHz");
        }

        if(show_frame && i == model->selected_index) {
            elements_frame(canvas, current_x - 2, current_y - 9, 63, 11);
        }
    }
}

void subghz_frequency_analyzer_draw(Canvas* canvas, SubGhzFrequencyAnalyzerModel* model) {
    char buffer[64] = {0};

    // Title
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    //canvas_draw_str(canvas, 0, 7, model->is_ext_radio ? "Ext" : "Int");
    canvas_draw_str(canvas, 20, 7, "Frequency Analyzer");

    // RSSI
    canvas_draw_str(canvas, 33, 62, "RSSI");
    subghz_frequency_analyzer_draw_rssi(
        canvas, model->rssi, model->rssi_last, model->trigger, 56, 57);

    // Frequency
    // The port's FontBigNumbers uses profont22, which exceeds the original
    // 19px highlight. Keep the complete live readout inside y=10..27.
    canvas_set_custom_u8g2_font(canvas, u8g2_font_profont17_tn);
    snprintf(
        buffer,
        sizeof(buffer),
        "%03lu.%03lu",
        (unsigned long)(model->frequency / 1000000 % 1000),
        (unsigned long)(model->frequency / 1000 % 1000));
    if(model->signal) {
        canvas_draw_box(canvas, 4, 10, 121, 18);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_set_color(canvas, ColorBlack);
    }

    canvas_draw_str(canvas, 8, 25, buffer);
    canvas_draw_icon(canvas, 96, 14, &I_MHz_25x11);

    canvas_set_color(canvas, ColorBlack);
    // Draw history after the live panel; its first selection frame starts at
    // y=28, immediately below the highlight, and can never be erased by it.
    subghz_frequency_analyzer_history_frequency_draw(canvas, model);
    canvas_set_font(canvas, FontSecondary);
    const uint8_t icon_x = 119;
    switch(model->feedback_level) {
    case SubGHzFrequencyAnalyzerFeedbackLevelAll:
        canvas_draw_icon(canvas, icon_x, 1, &I_Volup_8x6);
        break;
    case SubGHzFrequencyAnalyzerFeedbackLevelVibro:
        canvas_draw_icon(canvas, icon_x, 1, &I_Voldwn_6x6);
        break;
    case SubGHzFrequencyAnalyzerFeedbackLevelMute:
        canvas_draw_icon(canvas, icon_x, 1, &I_Voldwn_6x6);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 123, 1, 2, 6);
        canvas_set_color(canvas, ColorBlack);
        break;
    }

    // Buttons hint
    canvas_set_font(canvas, FontSecondary);
    elements_button_left(canvas, "T-");
    elements_button_right(canvas, "+T");
}

bool subghz_frequency_analyzer_input(InputEvent* event, void* context) {
    furi_assert(context);
    SubGhzFrequencyAnalyzer* instance = (SubGhzFrequencyAnalyzer*)context;

    bool need_redraw = false;
    if(event->key == InputKeyBack) {
        return need_redraw;
    }

    bool is_press_or_repeat = (event->type == InputTypePress) || (event->type == InputTypeRepeat);
    if(is_press_or_repeat && (event->key == InputKeyLeft || event->key == InputKeyRight)) {
        // Trigger setup
        float trigger_level = subghz_frequency_analyzer_worker_get_trigger_level(instance->worker);
        if(event->key == InputKeyLeft) {
            trigger_level -= TRIGGER_STEP;
            if(trigger_level < RSSI_MIN) {
                trigger_level = RSSI_MIN;
            }
        } else {
            trigger_level += TRIGGER_STEP;
            if(trigger_level > RSSI_MAX) {
                trigger_level = RSSI_MAX;
            }
        }
        subghz_frequency_analyzer_worker_set_trigger_level(instance->worker, trigger_level);
        FURI_LOG_D(TAG, "trigger = %.1f", (double)trigger_level);
        need_redraw = true;
    } else if(event->type == InputTypePress && event->key == InputKeyUp) {
        if(instance->feedback_level == SubGHzFrequencyAnalyzerFeedbackLevelAll) {
            instance->feedback_level = SubGHzFrequencyAnalyzerFeedbackLevelMute;
        } else {
            instance->feedback_level--;
        }

        need_redraw = true;
    } else if(is_press_or_repeat && event->key == InputKeyDown) {
        instance->show_frame = instance->max_index > 0;
        if(instance->show_frame) {
            instance->selected_index = (instance->selected_index + 1) % instance->max_index;
            need_redraw = true;
        }
    } else if(
        (event->type == InputTypeShort || event->type == InputTypeLong) &&
        event->key == InputKeyOk) {
        need_redraw = false;
        bool updated = false;
        uint32_t frequency_to_save;
        with_view_model(
            instance->view,
            SubGhzFrequencyAnalyzerModel * model,
            {
                frequency_to_save = model->frequency_to_save;
                uint32_t prev_freq_to_save = model->frequency_to_save;
                uint32_t frequency_candidate = 0;

                if(model->show_frame && !model->signal) {
                    frequency_candidate = model->history_frequency[model->selected_index];
                } else if(
                    (model->show_frame && model->signal) ||
                    (!model->show_frame && model->signal)) {
                    frequency_candidate = subghz_frequency_analyzer_get_nearest_frequency(
                        instance->worker, model->frequency);
                }

                frequency_candidate = frequency_candidate == 0 ||
                                              !subghz_txrx_radio_device_is_frequency_valid(
                                                  instance->txrx, frequency_candidate) ||
                                              prev_freq_to_save == frequency_candidate ?
                                          0 :
                                          subghz_frequency_analyzer_get_nearest_frequency(
                                              instance->worker, frequency_candidate);
                if(frequency_candidate > 0 && frequency_candidate != model->frequency_to_save) {
                    model->frequency_to_save = frequency_candidate;
                    frequency_to_save = frequency_candidate;
                    updated = true;
                }
            },
            false);

        if(updated) {
            instance->callback(SubGhzCustomEventViewFreqAnalOkShort, instance->context);
        }

        // If it was a long press also send a second event
        if(event->type == InputTypeLong && frequency_to_save > 0) {
            // Worker stopped on app thread instead of GUI thread when switching scene in callback

            instance->callback(SubGhzCustomEventViewFreqAnalOkLong, instance->context);
        }
    }

    if(need_redraw) {
        with_view_model(
            instance->view,
            SubGhzFrequencyAnalyzerModel * model,
            {
                model->rssi_last = instance->rssi_last;
                model->trigger =
                    subghz_frequency_analyzer_worker_get_trigger_level(instance->worker);
                model->feedback_level = instance->feedback_level;
                model->max_index = instance->max_index;
                model->show_frame = instance->show_frame;
                model->selected_index = instance->selected_index;
            },
            true);
    }

    return true;
}

uint32_t round_int(uint32_t value, uint8_t n) {
    // Round value
    uint8_t on = n;
    while(n--) {
        uint8_t i = value % 10;
        value /= 10;
        if(i >= 5) value++;
    }
    while(on--)
        value *= 10;
    return value;
}

static void subghz_frequency_analyzer_history_record(
    SubGhzFrequencyAnalyzerModel* model, uint32_t frequency) {
    if(!frequency) return;
    size_t found = MAX_HISTORY;
    for(size_t i = 0; i < MAX_HISTORY; i++) {
        if(model->history_frequency[i] == frequency) { found = i; break; }
    }
    uint8_t count = 1;
    if(found < MAX_HISTORY) {
        count = model->history_frequency_rx_count[found];
        if(count < UINT8_MAX) count++;
    }
    size_t last = found < MAX_HISTORY ? found : MAX_HISTORY - 1;
    for(size_t i = last; i > 0; i--) {
        model->history_frequency[i] = model->history_frequency[i - 1];
        model->history_frequency_rx_count[i] = model->history_frequency_rx_count[i - 1];
    }
    model->history_frequency[0] = frequency;
    model->history_frequency_rx_count[0] = count;
    model->max_index = 0;
    for(size_t i = 0; i < MAX_HISTORY; i++) {
        if(model->history_frequency[i]) model->max_index = i + 1;
    }
}

void subghz_frequency_analyzer_pair_callback(
    void* context,
    uint32_t frequency,
    float rssi,
    bool signal) {
    SubGhzFrequencyAnalyzer* instance = (SubGhzFrequencyAnalyzer*)context;
    bool has_rssi = !float_is_equal(rssi, 0.f);
    if(!has_rssi && instance->locked) {
        if(instance->callback) {
            instance->callback(SubGhzCustomEventSceneAnalyzerUnlock, instance->context);
        }
    } else if(has_rssi && !instance->locked) {
        instance->rssi_last = rssi;
        if(instance->callback) {
            instance->callback(SubGhzCustomEventSceneAnalyzerLock, instance->context);
        }
    }
    if(frequency) frequency = round_int(frequency, 3);
    uint32_t history_frequency = 0;
    if(signal && has_rssi && frequency) {
        history_frequency = subghz_frequency_analyzer_get_nearest_frequency(
            instance->worker, frequency);
        // Frequencies above the last configured preset can have no nearest
        // result. Never discard a valid worker detection just for that reason.
        if(!history_frequency) history_frequency = frequency;
    }
    if(rssi >= instance->rssi_last && frequency) instance->rssi_last = rssi;
    instance->locked = has_rssi;
    with_view_model(
        instance->view,
        SubGhzFrequencyAnalyzerModel * model,
        {
            // Record at detection, not after the worker's 20-scan hold expires.
            // Continuous transmitters then appear immediately too. Held samples
            // do not count twice; a new burst or frequency change records once.
            if(!has_rssi) model->history_active_frequency = 0;
            if(history_frequency && history_frequency != model->history_active_frequency) {
                subghz_frequency_analyzer_history_record(model, history_frequency);
                model->history_active_frequency = history_frequency;
                instance->max_index = model->max_index;
                instance->show_frame = true;
                instance->selected_index = 0;
            }
            model->rssi = rssi;
            model->rssi_last = instance->rssi_last;
            model->frequency = frequency;
            model->signal = signal;
            model->trigger = subghz_frequency_analyzer_worker_get_trigger_level(instance->worker);
            model->feedback_level = instance->feedback_level;
            model->max_index = instance->max_index;
            model->show_frame = instance->show_frame;
            model->selected_index = instance->selected_index;
        },
        true);
}

void subghz_frequency_analyzer_enter(void* context) {
    furi_assert(context);
    SubGhzFrequencyAnalyzer* instance = (SubGhzFrequencyAnalyzer*)context;

    //Start worker
    instance->worker = subghz_frequency_analyzer_worker_alloc(instance->context);

    subghz_frequency_analyzer_worker_set_pair_callback(
        instance->worker,
        (SubGhzFrequencyAnalyzerWorkerPairCallback)subghz_frequency_analyzer_pair_callback,
        instance);

    // Reset all state before starting RX so the first callback cannot be erased.
    instance->locked = false;
    instance->rssi_last = 0;
    instance->selected_index = 0;
    instance->max_index = 0;
    instance->show_frame = false;
    //subghz_frequency_analyzer_worker_set_trigger_level(instance->worker, RSSI_MIN);

    with_view_model(
        instance->view,
        SubGhzFrequencyAnalyzerModel * model,
        {
            model->selected_index = 0;
            model->max_index = 0;
            model->show_frame = false;
            model->rssi = 0;
            model->rssi_last = 0;
            model->frequency = 0;
            model->history_frequency[3] = 0;
            model->history_frequency[2] = 0;
            model->history_frequency[1] = 0;
            model->history_frequency[0] = 0;
            model->history_frequency_rx_count[3] = 0;
            model->history_frequency_rx_count[2] = 0;
            model->history_frequency_rx_count[1] = 0;
            model->history_frequency_rx_count[0] = 0;
            model->frequency_to_save = 0;
            model->history_active_frequency = 0;
            model->trigger = RSSI_MIN;
            model->is_ext_radio =
                (subghz_txrx_radio_device_get(instance->txrx) != SubGhzRadioDeviceTypeInternal);
        },
        true);
    subghz_frequency_analyzer_worker_start(instance->worker);
}

void subghz_frequency_analyzer_exit(void* context) {
    furi_assert(context);
    SubGhzFrequencyAnalyzer* instance = (SubGhzFrequencyAnalyzer*)context;

    // Stop worker
    if(subghz_frequency_analyzer_worker_is_running(instance->worker)) {
        subghz_frequency_analyzer_worker_stop(instance->worker);
    }
    subghz_frequency_analyzer_worker_free(instance->worker);

    furi_record_close(RECORD_NOTIFICATION);
}

SubGhzFrequencyAnalyzer* subghz_frequency_analyzer_alloc(SubGhzTxRx* txrx) {
    SubGhzFrequencyAnalyzer* instance = malloc(sizeof(SubGhzFrequencyAnalyzer));

    instance->feedback_level = SubGHzFrequencyAnalyzerFeedbackLevelMute;

    // View allocation and configuration
    instance->view = view_alloc();
    view_allocate_model(
        instance->view, ViewModelTypeLocking, sizeof(SubGhzFrequencyAnalyzerModel));
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, (ViewDrawCallback)subghz_frequency_analyzer_draw);
    view_set_input_callback(instance->view, subghz_frequency_analyzer_input);
    view_set_enter_callback(instance->view, subghz_frequency_analyzer_enter);
    view_set_exit_callback(instance->view, subghz_frequency_analyzer_exit);

    instance->txrx = txrx;

    return instance;
}

void subghz_frequency_analyzer_free(SubGhzFrequencyAnalyzer* instance) {
    furi_assert(instance);

    view_free(instance->view);
    free(instance);
}

View* subghz_frequency_analyzer_get_view(SubGhzFrequencyAnalyzer* instance) {
    furi_assert(instance);
    return instance->view;
}

uint32_t subghz_frequency_analyzer_get_frequency_to_save(SubGhzFrequencyAnalyzer* instance) {
    furi_assert(instance);
    uint32_t frequency;
    with_view_model(
        instance->view,
        SubGhzFrequencyAnalyzerModel * model,
        { frequency = model->frequency_to_save; },
        false);

    return frequency;
}

SubGHzFrequencyAnalyzerFeedbackLevel subghz_frequency_analyzer_feedback_level(
    SubGhzFrequencyAnalyzer* instance,
    SubGHzFrequencyAnalyzerFeedbackLevel level,
    bool update) {
    furi_assert(instance);
    if(update) {
        instance->feedback_level = level;
        with_view_model(
            instance->view,
            SubGhzFrequencyAnalyzerModel * model,
            { model->feedback_level = instance->feedback_level; },
            true);
    }

    return instance->feedback_level;
}

float subghz_frequency_analyzer_get_trigger_level(SubGhzFrequencyAnalyzer* instance) {
    furi_assert(instance);
    return subghz_frequency_analyzer_worker_get_trigger_level(instance->worker);
}
