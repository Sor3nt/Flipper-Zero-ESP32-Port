#include "expansion_settings_app.h"

#ifdef ESP_PLATFORM
#include <boards/board.h>
#endif

static const char* const expansion_uart_text[] = {
    "USART",
#if !defined(BOARD_HAS_EXPANSION_UART) || !BOARD_HAS_EXPANSION_UART
    "LPUART",
#endif
    "None",
};

static void expansion_settings_app_uart_changed(VariableItem* item) {
    ExpansionSettingsApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, expansion_uart_text[index]);

    if(index == 0) {
        app->settings.uart_index = FuriHalSerialIdUsart;
        expansion_set_listen_serial(app->expansion, FuriHalSerialIdUsart);
#if !defined(BOARD_HAS_EXPANSION_UART) || !BOARD_HAS_EXPANSION_UART
    } else if(index == 1) {
        app->settings.uart_index = FuriHalSerialIdLpuart;
        expansion_set_listen_serial(app->expansion, FuriHalSerialIdLpuart);
#endif
    } else {
        app->settings.uart_index = FuriHalSerialIdMax;
        expansion_disable(app->expansion);
    }
}

static uint32_t expansion_settings_app_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static ExpansionSettingsApp* expansion_settings_app_alloc(void) {
    ExpansionSettingsApp* app = malloc(sizeof(ExpansionSettingsApp));
    furi_check(app);

    expansion_settings_load(&app->settings);
#if defined(BOARD_HAS_EXPANSION_UART) && BOARD_HAS_EXPANSION_UART
    if(app->settings.uart_index != FuriHalSerialIdUsart &&
       app->settings.uart_index != FuriHalSerialIdMax) {
#else
    if(app->settings.uart_index > FuriHalSerialIdMax) {
#endif
        FURI_LOG_W("ExpansionSettingsApp", "Unsupported UART setting; using None");
        app->settings.uart_index = FuriHalSerialIdMax;
        expansion_settings_save(&app->settings);
    }

    app->gui = furi_record_open(RECORD_GUI);
    app->expansion = furi_record_open(RECORD_EXPANSION);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->var_item_list = variable_item_list_alloc();

    VariableItem* item;
    uint8_t value_index;
    item = variable_item_list_add(
        app->var_item_list,
        "Listen UART",
        COUNT_OF(expansion_uart_text),
        expansion_settings_app_uart_changed,
        app);
#if defined(BOARD_HAS_EXPANSION_UART) && BOARD_HAS_EXPANSION_UART
    value_index = app->settings.uart_index == FuriHalSerialIdUsart ? 0 : 1;
#else
    value_index = app->settings.uart_index;
#endif
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, expansion_uart_text[value_index]);

    view_set_previous_callback(
        variable_item_list_get_view(app->var_item_list), expansion_settings_app_exit);
    view_dispatcher_add_view(
        app->view_dispatcher,
        ExpansionSettingsViewVarItemList,
        variable_item_list_get_view(app->var_item_list));

    view_dispatcher_switch_to_view(app->view_dispatcher, ExpansionSettingsViewVarItemList);

    return app;
}

static void expansion_settings_app_free(ExpansionSettingsApp* app) {
    furi_assert(app);

    expansion_settings_save(&app->settings);

    view_dispatcher_remove_view(app->view_dispatcher, ExpansionSettingsViewVarItemList);
    variable_item_list_free(app->var_item_list);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_EXPANSION);
    furi_record_close(RECORD_GUI);

    free(app);
}

int32_t expansion_settings_app(void* p) {
    UNUSED(p);
    ExpansionSettingsApp* app = expansion_settings_app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    expansion_settings_app_free(app);
    return 0;
}
