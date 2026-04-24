#include "../bad_ble_app_i.h"

void bad_ble_scene_start_on_enter(void* context) {
    BadBleApp* app = context;
    
    submenu_add_item(
        app->submenu,
        "Run Script",
        0,
        bad_ble_scene_start_submenu_callback,
        app);
    
    submenu_add_item(
        app->submenu,
        "Settings",
        1,
        bad_ble_scene_start_submenu_callback,
        app);
    
    submenu_add_item(
        app->submenu,
        "Connect",
        2,
        bad_ble_scene_start_submenu_callback,
        app);
    
    view_dispatcher_switch_to_view(app->view_dispatcher, BadBleViewSubmenu);
}

bool bad_ble_scene_start_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void bad_ble_scene_start_on_exit(void* context) {
    BadBleApp* app = context;
    submenu_reset(app->submenu);
}

void bad_ble_scene_start_submenu_callback(void* context, uint32_t index) {
    BadBleApp* app = context;
    
    switch(index) {
        case 0:
            scene_manager_next_scene(app->scene_manager, BadBleSceneList);
            break;
        case 1:
            scene_manager_next_scene(app->scene_manager, BadBleSceneSettings);
            break;
        case 2:
            if(!ble_hid_is_active()) {
                ble_hid_start_advertising();
            }
            break;
    }
}
