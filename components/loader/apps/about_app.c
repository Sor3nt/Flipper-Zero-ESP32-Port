#include <furi.h>
#include <gui.h>
#include <furi_hal_board_info.h>
#include <toolbox/fw_version.h>
#include <view_dispatcher.h>
#include <widget.h>

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Widget* widget;
} AboutApp;

static uint32_t about_app_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

int32_t about_app(void* p) {
    UNUSED(p);

    AboutApp* app = malloc(sizeof(AboutApp));
    memset(app, 0, sizeof(AboutApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->widget = widget_alloc();

    /* Same layout as the stock firmware: project name and author first, then
     * the device this build runs on, then the firmware version. The device
     * name comes from the board header, so every board shows its own name. */
    widget_add_string_multiline_element(
        app->widget,
        64,
        10,
        AlignCenter,
        AlignTop,
        FontPrimary,
        FURI_ESP32_PROJECT_NAME);

    widget_add_string_multiline_element(
        app->widget,
        64,
        20,
        AlignCenter,
        AlignTop,
        FontSecondary,
        FURI_ESP32_PROJECT_AUTHOR);

    widget_add_string_multiline_element(
        app->widget,
        64,
        38,
        AlignCenter,
        AlignTop,
        FontSecondary,
        furi_hal_board_get_name());

    widget_add_string_multiline_element(
        app->widget,
        64,
        49,
        AlignCenter,
        AlignTop,
        FontSecondary,
        "FW: " FURI_ESP32_VERSION);

    View* widget_view = widget_get_view(app->widget);
    view_set_previous_callback(widget_view, about_app_exit);

    view_dispatcher_add_view(app->view_dispatcher, 0, widget_view);
    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);
    view_dispatcher_run(app->view_dispatcher);

    view_dispatcher_remove_view(app->view_dispatcher, 0);
    widget_free(app->widget);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);

    return 0;
}