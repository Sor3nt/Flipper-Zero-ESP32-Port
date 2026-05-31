#include <furi.h>
#include <gui/elements.h>
#include <input/input.h>

#include "desktop_view_usb_storage.h"

void desktop_usb_storage_set_callback(
    DesktopUsbStorageView* usb_storage,
    DesktopUsbStorageViewCallback callback,
    void* context) {
    furi_assert(usb_storage);
    furi_assert(callback);
    usb_storage->callback = callback;
    usb_storage->context = context;
}

void desktop_usb_storage_set_state(
    DesktopUsbStorageView* usb_storage,
    DesktopUsbStorageState state,
    const char* error_msg) {
    furi_assert(usb_storage);
    with_view_model(
        usb_storage->view,
        DesktopUsbStorageViewModel * model,
        {
            model->state = state;
            model->error_msg = error_msg;
        },
        true);
}

static void desktop_usb_storage_draw_callback(Canvas* canvas, void* model) {
    DesktopUsbStorageViewModel* m = model;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "USB-Storage");
    canvas_draw_line(canvas, 16, 13, 112, 13);

    canvas_set_font(canvas, FontSecondary);

    switch(m->state) {
    case DesktopUsbStorageStateInit:
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Mounting on PC ...");
        break;
    case DesktopUsbStorageStateActive:
        canvas_draw_str_aligned(
            canvas, 64, 22, AlignCenter, AlignCenter, "SD visible as USB drive");
        canvas_set_font(canvas, FontBatteryPercent);
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "Eject from PC first,");
        canvas_draw_str_aligned(canvas, 64, 46, AlignCenter, AlignCenter, "then press Disconnect.");
        canvas_set_font(canvas, FontSecondary);
        elements_button_left(canvas, "Disconnect");
        break;
    case DesktopUsbStorageStateError:
        canvas_draw_str_aligned(
            canvas, 64, 28, AlignCenter, AlignCenter, m->error_msg ? m->error_msg : "Fehler");
        elements_button_left(canvas, "Back");
        break;
    }
}

static bool desktop_usb_storage_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    DesktopUsbStorageView* usb_storage = context;

    if(event->type == InputTypeShort &&
       (event->key == InputKeyBack || event->key == InputKeyUp)) {
        usb_storage->callback(DesktopUsbStorageEventExit, usb_storage->context);
        return true;
    }

    /* Swallow everything else so the desktop underneath doesn't react. */
    return true;
}

View* desktop_usb_storage_get_view(DesktopUsbStorageView* usb_storage) {
    furi_assert(usb_storage);
    return usb_storage->view;
}

DesktopUsbStorageView* desktop_usb_storage_alloc(void) {
    DesktopUsbStorageView* usb_storage = malloc(sizeof(DesktopUsbStorageView));
    usb_storage->view = view_alloc();
    view_allocate_model(
        usb_storage->view, ViewModelTypeLocking, sizeof(DesktopUsbStorageViewModel));
    view_set_context(usb_storage->view, usb_storage);
    view_set_draw_callback(usb_storage->view, desktop_usb_storage_draw_callback);
    view_set_input_callback(usb_storage->view, desktop_usb_storage_input_callback);
    return usb_storage;
}

void desktop_usb_storage_free(DesktopUsbStorageView* usb_storage) {
    furi_assert(usb_storage);
    view_free(usb_storage->view);
    free(usb_storage);
}
