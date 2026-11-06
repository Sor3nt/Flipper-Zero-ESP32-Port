/*!
 *  @file flipper-xremote/views/xremote_custom_view.c
    @license This project is released under the GNU GPLv3 License
 *  @copyright (c) 2023 Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Custom layout UI and view functionality for remote buttons.
 */

#include "xremote_custom_view.h"
#include "../xremote_app.h"

static void xremote_custom_view_draw_vertical(Canvas* canvas, XRemoteViewModel* model) {
    XRemoteAppButtons* buttons = model->context;

    FuriString* text = model->hold && model->ok_pressed ? buttons->custom_ok_hold :
                                                          buttons->custom_ok;
    xremote_canvas_draw_button_wide(
        canvas, model->ok_pressed, 0, 27, furi_string_get_cstr(text), XRemoteIconEnter);

    text = model->hold && model->up_pressed ? buttons->custom_up_hold : buttons->custom_up;
    xremote_canvas_draw_button_wide(
        canvas, model->up_pressed, 0, 45, furi_string_get_cstr(text), XRemoteIconArrowUp);

    text = model->hold && model->down_pressed ? buttons->custom_down_hold : buttons->custom_down;
    xremote_canvas_draw_button_wide(
        canvas, model->down_pressed, 0, 63, furi_string_get_cstr(text), XRemoteIconArrowDown);

    text = model->hold && model->left_pressed ? buttons->custom_left_hold : buttons->custom_left;
    xremote_canvas_draw_button_wide(
        canvas, model->left_pressed, 0, 81, furi_string_get_cstr(text), XRemoteIconArrowLeft);

    text = model->hold && model->right_pressed ? buttons->custom_right_hold :
                                                 buttons->custom_right;
    xremote_canvas_draw_button_wide(
        canvas, model->right_pressed, 0, 99, furi_string_get_cstr(text), XRemoteIconArrowRight);
}

static void xremote_custom_view_draw_horizontal(Canvas* canvas, XRemoteViewModel* model) {
    XRemoteAppButtons* buttons = model->context;

    FuriString* text = model->hold && model->ok_pressed ? buttons->custom_ok_hold :
                                                          buttons->custom_ok;
    xremote_canvas_draw_button_wide(
        canvas, model->ok_pressed, 0, 7, furi_string_get_cstr(text), XRemoteIconEnter);

    text = model->hold && model->up_pressed ? buttons->custom_up_hold : buttons->custom_up;
    xremote_canvas_draw_button_wide(
        canvas, model->up_pressed, 0, 25, furi_string_get_cstr(text), XRemoteIconArrowUp);

    text = model->hold && model->down_pressed ? buttons->custom_down_hold : buttons->custom_down;
    xremote_canvas_draw_button_wide(
        canvas, model->down_pressed, 0, 43, furi_string_get_cstr(text), XRemoteIconArrowDown);

    text = model->hold && model->left_pressed ? buttons->custom_left_hold : buttons->custom_left;
    xremote_canvas_draw_button_wide(
        canvas, model->left_pressed, 64, 20, furi_string_get_cstr(text), XRemoteIconArrowLeft);

    text = model->hold && model->right_pressed ? buttons->custom_right_hold :
                                                 buttons->custom_right;
    xremote_canvas_draw_button_wide(
        canvas, model->right_pressed, 64, 38, furi_string_get_cstr(text), XRemoteIconArrowRight);
}

static void xremote_custom_view_draw_page_name(Canvas* canvas, ViewOrientation orientation) {
    Align align = orientation == ViewOrientationHorizontal ? AlignRight : AlignLeft;
    uint8_t x = orientation == ViewOrientationHorizontal ? canvas_width(canvas) : 0;
    uint8_t y = orientation == ViewOrientationHorizontal ? 10 : 12;
    elements_multiline_text_aligned(canvas, x, y, align, AlignTop, "Custom");
}

static void xremote_custom_view_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    XRemoteViewModel* model = context;
    XRemoteAppButtons* buttons = model->context;
    XRemoteAppContext* app_ctx = buttons->app_ctx;

    const char* exit_str = xremote_app_context_get_exit_str(app_ctx);
    ViewOrientation orientation = app_ctx->app_settings->orientation;

    XRemoteViewDrawFunction xremote_custom_view_draw_body;
    xremote_custom_view_draw_body = orientation == ViewOrientationVertical ?
                                        xremote_custom_view_draw_vertical :
                                        xremote_custom_view_draw_horizontal;

    xremote_canvas_draw_header(canvas, orientation, NULL);
    xremote_custom_view_draw_page_name(canvas, orientation);
    xremote_custom_view_draw_body(canvas, model);
    xremote_canvas_draw_exit_footer(canvas, orientation, exit_str);
}

/* Paradoxt D-pad: custom command for key index 0=OK, 1=Up, 2=Down, 3=Left, 4=Right. */
static FuriString* xremote_custom_view_dpad_command(XRemoteAppButtons* buttons, int index) {
    switch(index) {
    case 0:
        return buttons->custom_ok;
    case 1:
        return buttons->custom_up;
    case 2:
        return buttons->custom_down;
    case 3:
        return buttons->custom_left;
    case 4:
        return buttons->custom_right;
    default:
        return NULL;
    }
}

static void xremote_custom_view_process(XRemoteView* view, InputEvent* event) {
    with_view_model(
        xremote_view_get_view(view),
        XRemoteViewModel * model,
        {
            XRemoteAppButtons* buttons = xremote_view_get_context(view);
            if(buttons && buttons->app_ctx) {
                model->context = buttons;

                const int dpad_index = xremote_view_dpad_index(event);
                if(dpad_index != XREMOTE_DPAD_OFF) {
                    /* Paradoxt D-pad: the key itself sends its custom command. */
                    FuriString* custom = xremote_custom_view_dpad_command(buttons, dpad_index);
                    if(custom) {
                        xremote_view_model_select(model, (uint8_t)dpad_index);
                        xremote_view_press_button(
                            view, xremote_view_get_button_by_name(view, furi_string_get_cstr(custom)));
                    }
                } else
                /* T-Embed rotary: turn selects; center click sends the selected custom command. */
                if(event->type == InputTypePress &&
               (event->key == InputKeyUp || event->key == InputKeyLeft)) {
                    xremote_view_model_move_selection(model, -1, 5);
                } else if(event->type == InputTypePress &&
                      (event->key == InputKeyDown || event->key == InputKeyRight)) {
                    xremote_view_model_move_selection(model, 1, 5);
                } else if(event->type == InputTypeShort && event->key == InputKeyOk) {
                    FuriString* selected = NULL;
                    switch(model->selected) {
                    case 0: selected = buttons->custom_ok; break;
                    case 1: selected = buttons->custom_up; break;
                    case 2: selected = buttons->custom_down; break;
                    case 3: selected = buttons->custom_left; break;
                    default: selected = buttons->custom_right; break;
                    }
                    InfraredRemoteButton* button = xremote_view_get_button_by_name(
                        view, furi_string_get_cstr(selected));
                    xremote_view_press_button(view, button);
                }
            }
        },
        true);
}

static bool xremote_custom_view_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    XRemoteView* view = (XRemoteView*)context;
    XRemoteAppContext* app_ctx = xremote_view_get_app_context(view);
    XRemoteAppExit exit_behavior = app_ctx->app_settings->exit_behavior;

    if(event->key == InputKeyBack) {
        if(event->type == InputTypeShort && exit_behavior == XRemoteAppExitPress)
            return false;
        else if(event->type == InputTypeLong && exit_behavior == XRemoteAppExitHold)
            return false;
    }

    xremote_custom_view_process(view, event);
    return true;
}

XRemoteView* xremote_custom_view_alloc(void* app_ctx, void* model_ctx) {
    XRemoteView* view = xremote_view_alloc(
        app_ctx, xremote_custom_view_input_callback, xremote_custom_view_draw_callback);
    xremote_view_set_context(view, model_ctx, NULL);

    with_view_model(
        xremote_view_get_view(view),
        XRemoteViewModel * model,
        {
            model->context = model_ctx;
            model->up_pressed = false;
            model->down_pressed = false;
            model->left_pressed = false;
            model->right_pressed = false;
            model->back_pressed = false;
            model->ok_pressed = true;
            model->selected = 0;
            model->hold = false;
        },
        true);

    return view;
}
