#include "bw16_app_i.h"
#include <dialogs/dialogs.h>

static bool bw16_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    Bw16App* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool bw16_app_back_event_callback(void* context) {
    furi_assert(context);
    Bw16App* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void bw16_app_tick_event_callback(void* context) {
    furi_assert(context);
    Bw16App* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

void bw16_app_set_command(
    Bw16App* app,
    const char* title,
    const char* command,
    bool continuous) {
    app->console_title = title;
    strncpy(app->tx_command, command, BW16_TX_CMD_SIZE - 1);
    app->tx_command[BW16_TX_CMD_SIZE - 1] = '\0';
    app->console_continuous = continuous;
}

static Bw16App* bw16_app_alloc(void) {
    Bw16App* app = malloc(sizeof(Bw16App));
    if(!app) return NULL;
    memset(app, 0, sizeof(Bw16App));

    app->gui = furi_record_open(RECORD_GUI);

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&bw16_scene_handlers, app);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, bw16_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, bw16_app_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, bw16_app_tick_event_callback, 100);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, Bw16AppViewSubmenu, submenu_get_view(app->submenu));

    app->text_box = text_box_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, Bw16AppViewTextBox, text_box_get_view(app->text_box));

    app->widget = widget_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, Bw16AppViewWidget, widget_get_view(app->widget));

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->text_box_store = furi_string_alloc();
    furi_string_reserve(app->text_box_store, BW16_TEXT_BOX_STORE_SIZE);
    for(size_t i = 0; i < COUNT_OF(app->text_box_view); i++) {
        app->text_box_view[i] = furi_string_alloc();
        furi_string_reserve(app->text_box_view[i], BW16_TEXT_BOX_STORE_SIZE);
    }
    app->rx_line = furi_string_alloc();

    scene_manager_next_scene(app->scene_manager, Bw16SceneStart);

    return app;
}

static void bw16_app_free(Bw16App* app) {
    furi_assert(app);

    /* Stop and join the UART worker FIRST: its rx callback touches text_box_store /
     * rx_line / the list arrays under app->mutex, so nothing it uses may be freed while it
     * can still run. (uart is NULL if the shared UART was busy at startup.) */
    if(app->uart) bw16_uart_free(app->uart);

    view_dispatcher_remove_view(app->view_dispatcher, Bw16AppViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, Bw16AppViewTextBox);
    view_dispatcher_remove_view(app->view_dispatcher, Bw16AppViewWidget);

    submenu_free(app->submenu);
    text_box_free(app->text_box);
    widget_free(app->widget);

    furi_string_free(app->text_box_store);
    for(size_t i = 0; i < COUNT_OF(app->text_box_view); i++) {
        furi_string_free(app->text_box_view[i]);
    }
    furi_string_free(app->rx_line);
    furi_mutex_free(app->mutex);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);

    free(app);
}

int32_t bw16_r4tkn_app(void* p) {
    UNUSED(p);

    /* The BW16 is powered from the Flipper 5V pin, which on this board is the OTG rail.
     * Enable it (best effort; only works on battery) and restore the previous state on
     * exit so we never leave OTG on for light sleep. */
    bool otg_was_enabled = furi_hal_power_is_otg_enabled();
    uint8_t attempts = 0;
    while(!furi_hal_power_is_otg_enabled() && attempts++ < 5) {
        furi_hal_power_enable_otg();
        furi_delay_ms(10);
    }
    furi_delay_ms(200);

    Bw16App* app = bw16_app_alloc();
    if(!app) {
        FURI_LOG_E("Bw16", "out of memory allocating app state");
        if(furi_hal_power_is_otg_enabled() && !otg_was_enabled) {
            furi_hal_power_disable_otg();
        }
        return 0;
    }
    app->uart = bw16_uart_init(app);

    if(app->uart) {
        bw16_uart_set_handle_rx_data_cb(app->uart, bw16_rx_handle_data);
        view_dispatcher_run(app->view_dispatcher);
    } else {
        /* Shared expansion UART was busy — tell the user instead of crashing. */
        DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
        DialogMessage* message = dialog_message_alloc();
        dialog_message_set_header(message, "BW16: UART busy", 64, 2, AlignCenter, AlignTop);
        dialog_message_set_text(
            message,
            "Close WiFi Marauder,\nUSB-UART bridge or\nESP-Flasher first.",
            64,
            34,
            AlignCenter,
            AlignCenter);
        dialog_message_set_buttons(message, NULL, "OK", NULL);
        dialog_message_show(dialogs, message);
        dialog_message_free(message);
        furi_record_close(RECORD_DIALOGS);
    }

    bw16_app_free(app);

    if(furi_hal_power_is_otg_enabled() && !otg_was_enabled) {
        furi_hal_power_disable_otg();
    }

    return 0;
}
