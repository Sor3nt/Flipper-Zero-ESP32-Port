#include "bad_ble_app_i.h"

static void bad_ble_ble_state_callback(bool connected, void* context) {
    BadBleApp* app = context;
    app->connected = connected;
}

static BadBleApp* bad_ble_app_alloc(void) {
    BadBleApp* app = malloc(sizeof(BadBleApp));
    
    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    
    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&bad_ble_scene_handlers, app);
    
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, bad_ble_back_event_callback);
    
    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, BadBleViewSubmenu, submenu_get_view(app->submenu));
    
    app->popup = popup_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, BadBleViewPopup, popup_get_view(app->popup));
    
    app->var_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, BadBleViewSettings, 
        variable_item_list_get_view(app->var_item_list));
    
    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, BadBleViewTextInput, text_input_get_view(app->text_input));
    
    app->file_path = furi_string_alloc();
    app->script_path = furi_string_alloc();
    
    strcpy(app->settings.device_name, "BadBLE");
    
    BleHidConfig ble_config = {
        .device_name = "BadBLE",
        .bonding = true,
        .pairing = BleHidPairingModeVerifyYesNo,
    };
    
    ble_hid_get_default_mac(ble_config.mac);
    app->ble_hid = ble_hid_alloc(&ble_config);
    ble_hid_set_state_callback(app->ble_hid, bad_ble_ble_state_callback, app);
    
    app->is_running = false;
    app->connected = false;
    
    return app;
}

static void bad_ble_app_free(BadBleApp* app) {
    furi_assert(app);
    
    view_dispatcher_remove_view(app->view_dispatcher, BadBleViewSubmenu);
    submenu_free(app->submenu);
    
    view_dispatcher_remove_view(app->view_dispatcher, BadBleViewPopup);
    popup_free(app->popup);
    
    view_dispatcher_remove_view(app->view_dispatcher, BadBleViewSettings);
    variable_item_list_free(app->var_item_list);
    
    view_dispatcher_remove_view(app->view_dispatcher, BadBleViewTextInput);
    text_input_free(app->text_input);
    
    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);
    
    furi_string_free(app->file_path);
    furi_string_free(app->script_path);
    
    ble_hid_free(app->ble_hid);
    
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_STORAGE);
    
    free(app);
}

static bool bad_ble_back_event_callback(void* context) {
    furi_assert(context);
    BadBleApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

int32_t bad_ble_app(void* p) {
    UNUSED(p);
    
    BadBleApp* app = bad_ble_app_alloc();
    
    scene_manager_next_scene(app->scene_manager, BadBleSceneStart);
    
    view_dispatcher_run(app->view_dispatcher);
    
    bad_ble_app_free(app);
    
    return 0;
}
