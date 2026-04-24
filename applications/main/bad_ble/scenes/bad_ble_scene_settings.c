#include "../bad_ble_app_i.h"

void bad_ble_scene_settings_var_item_callback(VariableItem* item) {
    BadBleApp* app = variable_item_get_context(item);
    view_dispatcher_send_custom_event(app->view_dispatcher, variable_item_get_current_value_index(item));
}

static void bad_ble_scene_settings_text_result_callback(void* context) {
    BadBleApp* app = context;
    strcpy(app->settings.device_name, app->input_buffer);
    
    BleHidConfig new_config = {
        .bonding = true,
        .pairing = BleHidPairingModeVerifyYesNo,
    };
    strcpy(new_config.device_name, app->settings.device_name);
    ble_hid_get_default_mac(new_config.mac);
    
    ble_hid_free(app->ble_hid);
    app->ble_hid = ble_hid_alloc(&new_config);
    
    scene_manager_previous_scene(app->scene_manager);
}

void bad_ble_scene_settings_on_enter(void* context) {
    BadBleApp* app = context;
    
    variable_item_list_set_header(app->var_item_list, "BLE Settings");
    
    VariableItem* item = variable_item_list_add_item(
        app->var_item_list,
        "Device Name",
        1,
        bad_ble_scene_settings_var_item_callback,
        app);
    
    variable_item_set_current_value_text(item, app->settings.device_name);
    
    view_dispatcher_switch_to_view(app->view_dispatcher, BadBleViewSettings);
}

bool bad_ble_scene_settings_on_event(void* context, SceneManagerEvent event) {
    BadBleApp* app = context;
    
    if(event.type == SceneManagerEventTypeCustom) {
        strcpy(app->input_buffer, app->settings.device_name);
        text_input_set_result_callback(
            app->text_input,
            bad_ble_scene_settings_text_result_callback,
            app,
            app->input_buffer,
            sizeof(app->input_buffer) - 1,
            false);
        text_input_set_header_text(app->text_input, "Device Name");
        view_dispatcher_switch_to_view(app->view_dispatcher, BadBleViewTextInput);
        return true;
    }
    return false;
}

void bad_ble_scene_settings_on_exit(void* context) {
    BadBleApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
