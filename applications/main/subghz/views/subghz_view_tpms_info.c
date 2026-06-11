#include "subghz_view_tpms_info.h"
#include <lib/subghz/protocols/tpms_generic.h>
#include <gui/elements.h>
#include <input/input.h>
#include <furi.h>

struct SubGhzViewTpmsInfo {
    View* view;
    SubGhzViewTpmsInfoActionCallback callback;
    void* callback_context;
};

typedef struct {
    FuriString* protocol_name;
    FuriString* frequency;
    FuriString* modulation;
    TPMSBlockGeneric* generic;
    SubGhzViewTpmsField selected;
    bool can_send;
    bool can_save;
    bool tx_active; // OK long-held → invert Send button label
} SubGhzViewTpmsInfoModel;

void subghz_view_tpms_info_set_callback(
    SubGhzViewTpmsInfo* tpms_info,
    SubGhzViewTpmsInfoActionCallback callback,
    void* context) {
    furi_assert(tpms_info);
    tpms_info->callback = callback;
    tpms_info->callback_context = context;
}

void subghz_view_tpms_info_update(
    SubGhzViewTpmsInfo* tpms_info,
    FlipperFormat* fff,
    const char* frequency,
    const char* modulation,
    bool can_send,
    bool can_save) {
    furi_assert(tpms_info);
    furi_assert(fff);

    with_view_model(
        tpms_info->view,
        SubGhzViewTpmsInfoModel * model,
        {
            flipper_format_rewind(fff);
            flipper_format_read_string(fff, "Protocol", model->protocol_name);
            tpms_block_generic_deserialize(model->generic, fff);
            furi_string_set(model->frequency, frequency ? frequency : "");
            furi_string_set(model->modulation, modulation ? modulation : "");
            model->can_send = can_send;
            model->can_save = can_save;
            model->tx_active = false;
            model->selected = SubGhzViewTpmsFieldPressure;
        },
        true);
}

SubGhzViewTpmsField subghz_view_tpms_info_get_selected_field(SubGhzViewTpmsInfo* tpms_info) {
    furi_assert(tpms_info);
    SubGhzViewTpmsField sel = SubGhzViewTpmsFieldPressure;
    with_view_model(
        tpms_info->view,
        SubGhzViewTpmsInfoModel * model,
        { sel = model->selected; },
        false);
    return sel;
}

void subghz_view_tpms_info_set_values(
    SubGhzViewTpmsInfo* tpms_info,
    uint32_t id,
    float pressure,
    float temperature,
    uint8_t battery_low) {
    furi_assert(tpms_info);
    with_view_model(
        tpms_info->view,
        SubGhzViewTpmsInfoModel * model,
        {
            model->generic->id = id;
            model->generic->pressure = pressure;
            model->generic->temperature = temperature;
            model->generic->battery_low = battery_low;
        },
        true);
}

// Layout coordinates for the four field boxes (x, y, w, h). Used both for
// drawing and for the selection highlight.
static const struct {
    uint8_t x, y, w, h;
} field_boxes[SubGhzViewTpmsFieldCount] = {
    [SubGhzViewTpmsFieldPressure] = {0, 14, 70, 30},
    [SubGhzViewTpmsFieldTemperature] = {73, 14, 55, 14},
    [SubGhzViewTpmsFieldId] = {73, 31, 55, 13},
    [SubGhzViewTpmsFieldBattery] = {0, 46, 60, 12},
};

static void subghz_view_tpms_info_draw(Canvas* canvas, SubGhzViewTpmsInfoModel* model) {
    char buffer[32];
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    // Header line: protocol name + freq / modulation on the right
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 0, 8, furi_string_get_cstr(model->protocol_name));
    canvas_draw_str(canvas, 78, 8, furi_string_get_cstr(model->frequency));
    canvas_draw_str(canvas, 110, 8, furi_string_get_cstr(model->modulation));
    canvas_draw_line(canvas, 0, 11, 128, 11);

    // Big pressure box (left)
    elements_bold_rounded_frame(canvas, 0, 14, 70, 30);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 22, "Pressure");
    canvas_set_font(canvas, FontBigNumbers);
    snprintf(buffer, sizeof(buffer), "%2.1f", (double)model->generic->pressure);
    canvas_draw_str_aligned(canvas, 35, 35, AlignCenter, AlignCenter, buffer);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 50, 42, "bar");

    // Temperature box (right top)
    elements_bold_rounded_frame(canvas, 73, 14, 55, 14);
    canvas_set_font(canvas, FontSecondary);
    snprintf(buffer, sizeof(buffer), "Temp %2.0f", (double)model->generic->temperature);
    canvas_draw_str(canvas, 77, 23, buffer);
    canvas_draw_str(canvas, 121, 23, "C");
    canvas_draw_circle(canvas, 119, 18, 1); // °

    // ID box (right bottom)
    elements_bold_rounded_frame(canvas, 73, 31, 55, 13);
    canvas_draw_str(canvas, 77, 40, "ID");
    snprintf(buffer, sizeof(buffer), "%08lX", (unsigned long)model->generic->id);
    canvas_draw_str(canvas, 88, 40, buffer);

    // Battery box (bottom-left)
    elements_bold_rounded_frame(canvas, 0, 46, 60, 12);
    if(model->generic->battery_low == TPMS_NO_BATT) {
        canvas_draw_str(canvas, 3, 55, "Batt: --");
    } else {
        canvas_draw_str(
            canvas, 3, 55, model->generic->battery_low ? "Batt: low" : "Batt: ok");
    }

    // Highlight currently selected field with an inverted overlay
    if(model->selected < SubGhzViewTpmsFieldCount) {
        const uint8_t x = field_boxes[model->selected].x;
        const uint8_t y = field_boxes[model->selected].y;
        const uint8_t w = field_boxes[model->selected].w;
        const uint8_t h = field_boxes[model->selected].h;
        canvas_set_color(canvas, ColorBlack);
        // Draw a second frame inside the field to denote selection
        canvas_draw_rframe(canvas, x + 1, y + 1, w - 2, h - 2, 2);
    }

    // Bit-count hint right (below the field grid)
    snprintf(buffer, sizeof(buffer), "%d bit", model->generic->data_count_bit);
    canvas_draw_str_aligned(canvas, 128, 55, AlignRight, AlignBottom, buffer);

    // Action button hints along the bottom
    if(model->can_send) {
        elements_button_center(canvas, model->tx_active ? "Sending" : "Send");
    }
    if(model->can_save) {
        elements_button_right(canvas, "Save");
    }
}

static void subghz_view_tpms_info_fire(
    SubGhzViewTpmsInfo* tpms_info,
    SubGhzViewTpmsInfoAction action) {
    if(tpms_info->callback) {
        tpms_info->callback(action, tpms_info->callback_context);
    }
}

static bool subghz_view_tpms_info_input(InputEvent* event, void* context) {
    furi_assert(context);
    SubGhzViewTpmsInfo* tpms_info = context;

    // Back propagates to scene (closes view)
    if(event->key == InputKeyBack) return false;

    // Up/Down: wrap-around field navigation
    if(event->type == InputTypeShort && (event->key == InputKeyUp || event->key == InputKeyDown)) {
        with_view_model(
            tpms_info->view,
            SubGhzViewTpmsInfoModel * model,
            {
                int next = (int)model->selected +
                           (event->key == InputKeyDown ? 1 : (int)SubGhzViewTpmsFieldCount - 1);
                model->selected = (SubGhzViewTpmsField)(next % (int)SubGhzViewTpmsFieldCount);
            },
            true);
        return true;
    }

    bool can_send = false;
    bool can_save = false;
    SubGhzViewTpmsField sel = SubGhzViewTpmsFieldPressure;
    with_view_model(
        tpms_info->view,
        SubGhzViewTpmsInfoModel * model,
        {
            can_send = model->can_send;
            can_save = model->can_save;
            sel = model->selected;
        },
        false);

    if(event->key == InputKeyOk) {
        // Short press → Edit (or in-place toggle for Battery)
        if(event->type == InputTypeShort) {
            if(sel == SubGhzViewTpmsFieldBattery) {
                subghz_view_tpms_info_fire(tpms_info, SubGhzViewTpmsInfoActionToggleBattery);
            } else {
                subghz_view_tpms_info_fire(tpms_info, SubGhzViewTpmsInfoActionEdit);
            }
            return true;
        }
        // Long press → start endless TX
        if(event->type == InputTypeLong && can_send) {
            with_view_model(
                tpms_info->view,
                SubGhzViewTpmsInfoModel * model,
                { model->tx_active = true; },
                true);
            subghz_view_tpms_info_fire(tpms_info, SubGhzViewTpmsInfoActionTxStart);
            return true;
        }
        // Release after long → stop TX (only if we actually started)
        if(event->type == InputTypeRelease) {
            bool was_active = false;
            with_view_model(
                tpms_info->view,
                SubGhzViewTpmsInfoModel * model,
                {
                    was_active = model->tx_active;
                    model->tx_active = false;
                },
                true);
            if(was_active) {
                subghz_view_tpms_info_fire(tpms_info, SubGhzViewTpmsInfoActionTxStop);
            }
            return true;
        }
        return true;
    }

    if(event->key == InputKeyRight && can_save && event->type == InputTypeShort) {
        subghz_view_tpms_info_fire(tpms_info, SubGhzViewTpmsInfoActionSave);
        return true;
    }

    return true;
}

SubGhzViewTpmsInfo* subghz_view_tpms_info_alloc(void) {
    SubGhzViewTpmsInfo* tpms_info = malloc(sizeof(SubGhzViewTpmsInfo));

    tpms_info->callback = NULL;
    tpms_info->callback_context = NULL;
    tpms_info->view = view_alloc();
    view_allocate_model(
        tpms_info->view, ViewModelTypeLocking, sizeof(SubGhzViewTpmsInfoModel));
    view_set_context(tpms_info->view, tpms_info);
    view_set_draw_callback(tpms_info->view, (ViewDrawCallback)subghz_view_tpms_info_draw);
    view_set_input_callback(tpms_info->view, subghz_view_tpms_info_input);

    with_view_model(
        tpms_info->view,
        SubGhzViewTpmsInfoModel * model,
        {
            model->generic = malloc(sizeof(TPMSBlockGeneric));
            model->protocol_name = furi_string_alloc();
            model->frequency = furi_string_alloc();
            model->modulation = furi_string_alloc();
            model->can_send = false;
            model->can_save = false;
            model->tx_active = false;
            model->selected = SubGhzViewTpmsFieldPressure;
        },
        true);

    return tpms_info;
}

void subghz_view_tpms_info_free(SubGhzViewTpmsInfo* tpms_info) {
    furi_assert(tpms_info);

    with_view_model(
        tpms_info->view,
        SubGhzViewTpmsInfoModel * model,
        {
            furi_string_free(model->protocol_name);
            furi_string_free(model->frequency);
            furi_string_free(model->modulation);
            free(model->generic);
        },
        false);

    view_free(tpms_info->view);
    free(tpms_info);
}

View* subghz_view_tpms_info_get_view(SubGhzViewTpmsInfo* tpms_info) {
    furi_assert(tpms_info);
    return tpms_info->view;
}
