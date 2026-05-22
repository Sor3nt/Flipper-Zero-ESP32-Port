#include <furi.h>
#include <gui/elements.h>
#include <input/input.h>

#include "desktop_view_bruce_confirm.h"

void desktop_bruce_confirm_set_callback(
    DesktopBruceConfirmView* bruce_confirm,
    DesktopBruceConfirmViewCallback callback,
    void* context) {
    furi_assert(bruce_confirm);
    furi_assert(callback);
    bruce_confirm->callback = callback;
    bruce_confirm->context = context;
}

static void desktop_bruce_confirm_draw_callback(Canvas* canvas, void* model) {
    UNUSED(model);

    canvas_clear(canvas);

    const char* title = "Switch Firmware";
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, title);
    uint16_t tw = canvas_string_width(canvas, title);
    canvas_draw_line(canvas, 64 - tw / 2, 13, 64 + tw / 2, 13);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Reboot into Bruce ?");

    /* T-Embed encoder: Up/Down via rotation, so map the choice to those. */
    elements_button_up(canvas, "Cancel");
    elements_button_down(canvas, "Load");
}

static bool desktop_bruce_confirm_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    DesktopBruceConfirmView* bruce_confirm = context;

    if(event->type != InputTypeShort) return true;

    if(event->key == InputKeyDown) {
        bruce_confirm->callback(DesktopBruceEventLoad, bruce_confirm->context);
    } else {
        /* Up, Back, Ok — anything else cancels. */
        bruce_confirm->callback(DesktopBruceEventCancel, bruce_confirm->context);
    }
    return true;
}

View* desktop_bruce_confirm_get_view(DesktopBruceConfirmView* bruce_confirm) {
    furi_assert(bruce_confirm);
    return bruce_confirm->view;
}

DesktopBruceConfirmView* desktop_bruce_confirm_alloc(void) {
    DesktopBruceConfirmView* bruce_confirm = malloc(sizeof(DesktopBruceConfirmView));
    bruce_confirm->view = view_alloc();
    view_set_context(bruce_confirm->view, bruce_confirm);
    view_set_draw_callback(bruce_confirm->view, desktop_bruce_confirm_draw_callback);
    view_set_input_callback(bruce_confirm->view, desktop_bruce_confirm_input_callback);
    return bruce_confirm;
}

void desktop_bruce_confirm_free(DesktopBruceConfirmView* bruce_confirm) {
    furi_assert(bruce_confirm);
    view_free(bruce_confirm->view);
    free(bruce_confirm);
}
