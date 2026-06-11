#include "notification_settings_color_picker.h"

#include <furi.h>
#include <furi_hal_display.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <stdio.h>

/* Cursor fields: the three channels, then the Save button. */
#define FIELD_R     0
#define FIELD_G     1
#define FIELD_B     2
#define FIELD_SAVE  3
#define FIELD_COUNT 4

/* Up/Down (rotary) edits in coarse steps; Left/Right (encoder-button + rotate)
 * fine-tunes by 1 so any exact value is still reachable. */
#define STEP_COARSE 5
#define STEP_FINE   1

typedef struct {
    uint8_t channel[3]; /* R, G, B (0-255) */
    uint8_t cursor;     /* FIELD_* */
    bool editing;       /* true = rotary changes the selected channel value */
} NotificationColorPickerModel;

struct NotificationColorPicker {
    View* view;
    NotificationColorPickerCallback callback;
    void* context;
};

/* RGB888 -> RGB565 with the ESP32-S3 SPI byte swap. Mirrors ui_color_pack_swap()
 * in components/notification/notification.c. */
static inline uint16_t color_picker_pack_swap(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
    return ((v & 0xFF) << 8) | (v >> 8);
}

/* Push the current color to the LCD background tint for a live full-screen
 * preview. The picker's own drawn elements stay in the UI foreground tint. */
static void color_picker_push_preview(uint8_t r, uint8_t g, uint8_t b) {
    furi_hal_display_set_fg_color(color_picker_pack_swap(r, g, b));
}

static void color_picker_adjust(NotificationColorPickerModel* model, int delta) {
    if(model->cursor > FIELD_B) return; /* Save field has no value */
    int v = (int)model->channel[model->cursor] + delta;
    if(v < 0) v = 0;
    if(v > 255) v = 255;
    model->channel[model->cursor] = (uint8_t)v;
}

static void color_picker_draw_callback(Canvas* canvas, void* _model) {
    NotificationColorPickerModel* model = _model;
    static const char* const labels[3] = {"R", "G", "B"};

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Custom Color");

    canvas_set_font(canvas, FontSecondary);

    const uint8_t row_h = 12;
    const uint8_t bar_x = 16;
    const uint8_t bar_w = 80;
    for(uint8_t i = 0; i < 3; i++) {
        uint8_t y = 13 + i * row_h;
        bool selected = (model->cursor == i);
        bool editing = selected && model->editing;

        if(selected) {
            canvas_draw_rframe(canvas, 0, y, 128, row_h, 2);
        }
        if(editing) {
            /* Double border = "rotate to change this channel". */
            canvas_draw_rframe(canvas, 2, y + 2, 124, row_h - 4, 2);
        }

        canvas_draw_str(canvas, 4, y + 9, labels[i]);

        canvas_draw_frame(canvas, bar_x, y + 2, bar_w, 7);
        uint8_t fill = (uint8_t)(((uint16_t)model->channel[i] * (bar_w - 2)) / 255);
        if(fill > 0) {
            canvas_draw_box(canvas, bar_x + 1, y + 3, fill, 5);
        }

        char num[6];
        snprintf(num, sizeof(num), "%u", (unsigned)model->channel[i]);
        canvas_draw_str_aligned(canvas, 124, y + 9, AlignRight, AlignBottom, num);
    }

    /* Bottom row: hex readout + Save button. */
    const uint8_t y = 13 + 3 * row_h; /* 49 */
    char hex[10];
    snprintf(
        hex,
        sizeof(hex),
        "#%02X%02X%02X",
        model->channel[0],
        model->channel[1],
        model->channel[2]);
    canvas_draw_str(canvas, 4, y + 10, hex);

    const uint8_t btn_x = 84, btn_y = y + 1, btn_w = 42, btn_h = 12;
    if(model->cursor == FIELD_SAVE) {
        canvas_draw_rbox(canvas, btn_x, btn_y, btn_w, btn_h, 2);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str_aligned(
            canvas, btn_x + btn_w / 2, btn_y + btn_h - 2, AlignCenter, AlignBottom, "Save");
        canvas_set_color(canvas, ColorBlack);
    } else {
        canvas_draw_rframe(canvas, btn_x, btn_y, btn_w, btn_h, 2);
        canvas_draw_str_aligned(
            canvas, btn_x + btn_w / 2, btn_y + btn_h - 2, AlignCenter, AlignBottom, "Save");
    }
}

static bool color_picker_input_callback(InputEvent* event, void* context) {
    NotificationColorPicker* picker = context;
    bool consumed = false;

    /* React only to discrete presses. The rotary emits Short per detent; held
     * keys (side-key Back) emit Repeat. Encoder long-press (Ok/Long) is ignored. */
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) {
        return false;
    }

    bool do_confirm = false;
    bool do_cancel = false;
    uint32_t rgb = 0;
    NotificationColorPickerCallback cb = NULL;
    void* cb_ctx = NULL;

    with_view_model(
        picker->view,
        NotificationColorPickerModel * model,
        {
            switch(event->key) {
            case InputKeyBack:
                do_cancel = true;
                consumed = true;
                break;
            case InputKeyOk:
                if(model->editing) {
                    model->editing = false; /* commit edit */
                } else if(model->cursor == FIELD_SAVE) {
                    rgb = ((uint32_t)model->channel[0] << 16) |
                          ((uint32_t)model->channel[1] << 8) | (uint32_t)model->channel[2];
                    do_confirm = true;
                } else {
                    model->editing = true; /* start editing this channel */
                }
                consumed = true;
                break;
            case InputKeyDown:
                if(model->editing) {
                    color_picker_adjust(model, +STEP_COARSE);
                } else {
                    model->cursor = (model->cursor + 1) % FIELD_COUNT;
                }
                consumed = true;
                break;
            case InputKeyUp:
                if(model->editing) {
                    color_picker_adjust(model, -STEP_COARSE);
                } else {
                    model->cursor = (model->cursor + FIELD_COUNT - 1) % FIELD_COUNT;
                }
                consumed = true;
                break;
            case InputKeyRight:
                if(model->editing) color_picker_adjust(model, +STEP_FINE);
                consumed = true;
                break;
            case InputKeyLeft:
                if(model->editing) color_picker_adjust(model, -STEP_FINE);
                consumed = true;
                break;
            default:
                break;
            }

            /* Keep the live preview in sync with any value change. */
            color_picker_push_preview(model->channel[0], model->channel[1], model->channel[2]);

            cb = picker->callback;
            cb_ctx = picker->context;
        },
        true);

    /* Invoke the result callback outside the model lock — it switches views
     * and mutates the (other) settings list. */
    if((do_confirm || do_cancel) && cb) {
        cb(cb_ctx, rgb, do_confirm);
    }

    return consumed;
}

NotificationColorPicker* notification_color_picker_alloc(void) {
    NotificationColorPicker* picker = malloc(sizeof(NotificationColorPicker));
    picker->callback = NULL;
    picker->context = NULL;

    picker->view = view_alloc();
    view_set_context(picker->view, picker);
    view_allocate_model(
        picker->view, ViewModelTypeLocking, sizeof(NotificationColorPickerModel));
    view_set_draw_callback(picker->view, color_picker_draw_callback);
    view_set_input_callback(picker->view, color_picker_input_callback);

    with_view_model(
        picker->view,
        NotificationColorPickerModel * model,
        {
            model->channel[0] = 0xFF;
            model->channel[1] = 0x80;
            model->channel[2] = 0x00;
            model->cursor = FIELD_R;
            model->editing = false;
        },
        false);

    return picker;
}

void notification_color_picker_free(NotificationColorPicker* picker) {
    furi_check(picker);
    view_free(picker->view);
    free(picker);
}

View* notification_color_picker_get_view(NotificationColorPicker* picker) {
    furi_check(picker);
    return picker->view;
}

void notification_color_picker_set_callback(
    NotificationColorPicker* picker,
    NotificationColorPickerCallback callback,
    void* context) {
    furi_check(picker);
    picker->callback = callback;
    picker->context = context;
}

void notification_color_picker_set_color(NotificationColorPicker* picker, uint32_t rgb) {
    furi_check(picker);
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;
    with_view_model(
        picker->view,
        NotificationColorPickerModel * model,
        {
            model->channel[0] = r;
            model->channel[1] = g;
            model->channel[2] = b;
            model->cursor = FIELD_R;
            model->editing = false;
        },
        true);
    color_picker_push_preview(r, g, b);
}
