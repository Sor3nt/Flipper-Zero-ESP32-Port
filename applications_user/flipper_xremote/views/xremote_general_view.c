/*!
 *  @file flipper-xremote/views/xremote_general_view.c
    @license This project is released under the GNU GPLv3 License
 *  @copyright (c) 2023 Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief General remote buttons section UI and view functionality.
 */

#include "xremote_general_view.h"
#include "../xremote_app.h"

static void xremote_general_view_draw_vertical(Canvas* canvas, XRemoteViewModel* model) {
    xremote_canvas_draw_button_wide(canvas, model->ok_pressed, 0, 27, "Power", XRemoteIconEnter);
    xremote_canvas_draw_button_wide(canvas, model->up_pressed, 0, 45, "Input", XRemoteIconArrowUp);
    xremote_canvas_draw_button_wide(
        canvas, model->down_pressed, 0, 63, "Setup", XRemoteIconArrowDown);
    xremote_canvas_draw_button_wide(
        canvas, model->left_pressed, 0, 81, "Menu", XRemoteIconArrowLeft);
    xremote_canvas_draw_button_wide(
        canvas, model->right_pressed, 0, 99, "List", XRemoteIconArrowRight);
}

static void xremote_general_view_draw_horizontal(Canvas* canvas, XRemoteViewModel* model) {
    xremote_canvas_draw_button_wide(canvas, model->ok_pressed, 0, 7, "Power", XRemoteIconEnter);
    xremote_canvas_draw_button_wide(canvas, model->up_pressed, 0, 25, "Input", XRemoteIconArrowUp);
    xremote_canvas_draw_button_wide(
        canvas, model->down_pressed, 0, 43, "Setup", XRemoteIconArrowDown);
    xremote_canvas_draw_button_wide(
        canvas, model->left_pressed, 64, 20, "Menu", XRemoteIconArrowLeft);
    xremote_canvas_draw_button_wide(
        canvas, model->right_pressed, 64, 38, "List", XRemoteIconArrowRight);
}

static void xremote_general_view_draw_page_name(Canvas* canvas, ViewOrientation orientation) {
    Align align = orientation == ViewOrientationHorizontal ? AlignRight : AlignLeft;
    uint8_t x = orientation == ViewOrientationHorizontal ? 128 : 0;
    uint8_t y = orientation == ViewOrientationHorizontal ? 10 : 12;
    elements_multiline_text_aligned(canvas, x, y, align, AlignTop, "General");
}

static void xremote_general_view_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    XRemoteViewModel* model = context;
    XRemoteAppContext* app_ctx = model->context;

    const char* exit_str = xremote_app_context_get_exit_str(app_ctx);
    ViewOrientation orientation = app_ctx->app_settings->orientation;

    XRemoteViewDrawFunction xremote_general_view_draw_body;
    xremote_general_view_draw_body = orientation == ViewOrientationVertical ?
                                         xremote_general_view_draw_vertical :
                                         xremote_general_view_draw_horizontal;

    xremote_canvas_draw_header(canvas, orientation, NULL);
    xremote_general_view_draw_page_name(canvas, orientation);
    xremote_general_view_draw_body(canvas, model);
    xremote_canvas_draw_exit_footer(canvas, orientation, exit_str);
}

static void xremote_general_view_process(XRemoteView* view, InputEvent* event) {
    static const char* const commands[] = {
        XREMOTE_COMMAND_POWER,
        XREMOTE_COMMAND_INPUT,
        XREMOTE_COMMAND_SETUP,
        XREMOTE_COMMAND_MENU,
        XREMOTE_COMMAND_LIST,
    };

    with_view_model(
        xremote_view_get_view(view),
        XRemoteViewModel * model,
        {
            XRemoteAppContext* app_ctx = xremote_view_get_app_context(view);
            model->context = app_ctx;

            const int dpad_index = xremote_view_dpad_index(event);
            if(dpad_index != XREMOTE_DPAD_OFF) {
                /* Paradoxt D-pad: the key itself sends its command. */
                if(dpad_index >= 0 && dpad_index < 5) {
                    xremote_view_model_select(model, (uint8_t)dpad_index);
                    xremote_view_press_button(
                        view, xremote_view_get_button_by_name(view, commands[dpad_index]));
                }
            } else
            /* T-Embed rotary: turn = change selected button; center click = send. */
            if(event->type == InputTypePress &&
               (event->key == InputKeyUp || event->key == InputKeyLeft)) {
                xremote_view_model_move_selection(model, -1, 5);
            } else if(event->type == InputTypePress &&
                      (event->key == InputKeyDown || event->key == InputKeyRight)) {
                xremote_view_model_move_selection(model, 1, 5);
            } else if(event->type == InputTypeShort && event->key == InputKeyOk) {
                InfraredRemoteButton* button =
                    xremote_view_get_button_by_name(view, commands[model->selected]);
                xremote_view_press_button(view, button);
            }
        },
        true);
}

static bool xremote_general_view_input_callback(InputEvent* event, void* context) {
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

    xremote_general_view_process(view, event);
    return true;
}

XRemoteView* xremote_general_view_alloc(void* app_ctx) {
    XRemoteView* view = xremote_view_alloc(
        app_ctx, xremote_general_view_input_callback, xremote_general_view_draw_callback);

    with_view_model(
        xremote_view_get_view(view),
        XRemoteViewModel * model,
        {
            model->context = app_ctx;
            model->up_pressed = false;
            model->down_pressed = false;
            model->left_pressed = false;
            model->right_pressed = false;
            model->back_pressed = false;
            model->ok_pressed = true;
            model->selected = 0;
        },
        true);

    return view;
}