#include "../esp_flasher_app_i.h"

static const char* const reset_mode_text[EspFlasherResetModeCount] = {
    [EspFlasherResetModeOff] = "Off",
    [EspFlasherResetModeEsptool] = "esptool",
    [EspFlasherResetModeFlipper] = "Flipper",
};

static const char* const fast_baud_text[EspFlasherFastBaudCount] = {
    [EspFlasherFastBaud921600] = "921600",
    [EspFlasherFastBaud460800] = "460800",
    [EspFlasherFastBaud230400] = "230400",
};

static void esp_flasher_scene_settings_reset_mode_show_error(EspFlasherApp* app) {
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "Reset pins", 64, 4, AlignCenter, AlignTop);
    dialog_message_set_text(
        message, "EN/IO0 pin used by\nanother feature", 64, 32, AlignCenter, AlignCenter);
    dialog_message_set_buttons(message, NULL, "OK", NULL);
    dialog_message_show(app->dialogs, message);
    dialog_message_free(message);
}

static void esp_flasher_scene_settings_reset_mode_changed(VariableItem* item) {
    EspFlasherApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    const uint8_t previous = app->settings.reset_mode;
    variable_item_set_current_value_text(item, reset_mode_text[index]);
    app->settings.reset_mode = index;
    // park the lines at the new idle level right away
    if(!esp_flasher_reset_lines_init(index)) {
        /* A header pin is claimed elsewhere (e.g. an attached external Sub-GHz/NRF24
         * module): keep the previously working mode selected instead of showing a mode
         * that is not actually driving anything. */
        app->settings.reset_mode = previous;
        variable_item_set_current_value_index(item, previous);
        variable_item_set_current_value_text(item, reset_mode_text[previous]);
        esp_flasher_scene_settings_reset_mode_show_error(app);
    }
}

static void esp_flasher_scene_settings_fast_baud_changed(VariableItem* item) {
    EspFlasherApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, fast_baud_text[index]);
    app->settings.fast_baud = index;
}

void esp_flasher_scene_settings_on_enter(void* context) {
    EspFlasherApp* app = context;
    VariableItemList* list = app->var_item_list;
    VariableItem* item;

    if(esp_flasher_reset_available()) {
        item = variable_item_list_add(
            list,
            "Reset pins",
            EspFlasherResetModeCount,
            esp_flasher_scene_settings_reset_mode_changed,
            app);
        variable_item_set_current_value_index(item, app->settings.reset_mode);
        variable_item_set_current_value_text(item, reset_mode_text[app->settings.reset_mode]);
#if ESP_FLASHER_HAS_RESET_PINS
        item = variable_item_list_add(list, "EN / IO0", 1, NULL, app);
        variable_item_set_current_value_text(
            item,
            "GPIO" ESP_FLASHER_STR(ESP_FLASHER_EN_GPIO) " / " ESP_FLASHER_STR(
                ESP_FLASHER_IO0_GPIO));
#endif
    } else {
        item = variable_item_list_add(list, "Reset pins", 1, NULL, app);
        variable_item_set_current_value_text(item, "N/A");
    }

    item = variable_item_list_add(
        list,
        "Fast baud",
        EspFlasherFastBaudCount,
        esp_flasher_scene_settings_fast_baud_changed,
        app);
    variable_item_set_current_value_index(item, app->settings.fast_baud);
    variable_item_set_current_value_text(item, fast_baud_text[app->settings.fast_baud]);

    view_dispatcher_switch_to_view(app->view_dispatcher, EspFlasherAppViewVarItemList);
}

bool esp_flasher_scene_settings_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void esp_flasher_scene_settings_on_exit(void* context) {
    EspFlasherApp* app = context;
    variable_item_list_reset(app->var_item_list);
    esp_flasher_app_save_settings(app);
}
