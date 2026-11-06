/*!
 *  @file flipper-xremote/views/xremote_control_view.c
    @license This project is released under the GNU GPLv3 License
 *  @copyright (c) 2023 Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief UI and view functionality for the IR control page.
 */

#include "xremote_control_view.h"
#include "../xremote_app.h"

static void xremote_control_view_draw_vertical(Canvas* canvas, XRemoteViewModel* model) {
    XRemoteAppContext* app_ctx = model->context;

    xremote_canvas_draw_button_png(canvas, model->up_pressed, 23, 30, &I_Chanup_Icon_11x11);
    xremote_canvas_draw_button_png(canvas, model->down_pressed, 23, 72, &I_Chandown_Icon_11x11);
    xremote_canvas_draw_button_png(canvas, model->left_pressed, 2, 51, &I_Voldown_Icon_11x11);
    xremote_canvas_draw_button_png(canvas, model->right_pressed, 44, 51, &I_Volup_Icon_11x11);
    xremote_canvas_draw_button_png(canvas, model->back_pressed, 2, 95, &I_Mute_Icon_11x11);
    xremote_canvas_draw_button(canvas, model->ok_pressed, 23, 51, XRemoteIconPlayPause);

    if(app_ctx->app_settings->exit_behavior == XRemoteAppExitPress)
        canvas_draw_icon(canvas, 22, 107, &I_Hold_Text_17x4);
}

static void xremote_control_view_draw_horizontal(Canvas* canvas, XRemoteViewModel* model) {
    XRemoteAppContext* app_ctx = model->context;

    xremote_canvas_draw_button_png(canvas, model->up_pressed, 23, 2, &I_Chanup_Icon_11x11);
    xremote_canvas_draw_button_png(canvas, model->down_pressed, 23, 44, &I_Chandown_Icon_11x11);
    xremote_canvas_draw_button_png(canvas, model->left_pressed, 2, 23, &I_Voldown_Icon_11x11);
    xremote_canvas_draw_button_png(canvas, model->right_pressed, 44, 23, &I_Volup_Icon_11x11);
    xremote_canvas_draw_button_png(canvas, model->back_pressed, 70, 33, &I_Mute_Icon_11x11);
    xremote_canvas_draw_button(canvas, model->ok_pressed, 23, 23, XRemoteIconPlayPause);

    if(app_ctx->app_settings->exit_behavior == XRemoteAppExitPress)
        canvas_draw_icon(canvas, 90, 45, &I_Hold_Text_17x4);
}

static void xremote_control_view_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    XRemoteViewModel* model = context;
    XRemoteAppContext* app_ctx = model->context;
    ViewOrientation orientation = app_ctx->app_settings->orientation;
    const char* exit_str = xremote_app_context_get_exit_str(app_ctx);

    XRemoteViewDrawFunction xremote_control_view_draw_body;
    xremote_control_view_draw_body = orientation == ViewOrientationVertical ?
                                         xremote_control_view_draw_vertical :
                                         xremote_control_view_draw_horizontal;

    xremote_canvas_draw_header(canvas, orientation, "Control");
    xremote_control_view_draw_body(canvas, model);
    xremote_canvas_draw_exit_footer(canvas, orientation, exit_str);
}

static void xremote_control_view_process(XRemoteView* view, InputEvent* event) {
    static const char* const commands[] = {
        XREMOTE_COMMAND_PLAY_PAUSE,
        XREMOTE_COMMAND_NEXT_CHAN,
        XREMOTE_COMMAND_PREV_CHAN,
        XREMOTE_COMMAND_VOL_DOWN,
        XREMOTE_COMMAND_VOL_UP,
        XREMOTE_COMMAND_MUTE,
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
                if(dpad_index >= 0 && dpad_index < 6) {
                    xremote_view_model_select(model, (uint8_t)dpad_index);
                    xremote_view_press_button(
                        view, xremote_view_get_button_by_name(view, commands[dpad_index]));
                }
            } else
            /* T-Embed rotary: turn = change selected button; center click = send. */
            if(event->type == InputTypePress &&
               (event->key == InputKeyUp || event->key == InputKeyLeft)) {
                xremote_view_model_move_selection(model, -1, 6);
            } else if(event->type == InputTypePress &&
                      (event->key == InputKeyDown || event->key == InputKeyRight)) {
                xremote_view_model_move_selection(model, 1, 6);
            } else if(event->type == InputTypeShort && event->key == InputKeyOk) {
                InfraredRemoteButton* button =
                    xremote_view_get_button_by_name(view, commands[model->selected]);
                xremote_view_press_button(view, button);
            }
        },
        true);
}

static bool xremote_control_view_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    XRemoteView* view = (XRemoteView*)context;
    XRemoteAppContext* app_ctx = xremote_view_get_app_context(view);
    XRemoteAppExit exit = app_ctx->app_settings->exit_behavior;

    if(event->key == InputKeyBack && event->type == InputTypeShort && exit == XRemoteAppExitPress)
        return false;
    else if(event->key == InputKeyBack && event->type == InputTypeLong && exit == XRemoteAppExitHold)
        return false;

    xremote_control_view_process(view, event);
    return true;
}

XRemoteView* xremote_control_view_alloc(void* app_ctx) {
    XRemoteView* view = xremote_view_alloc(
        app_ctx, xremote_control_view_input_callback, xremote_control_view_draw_callback);

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