#include "../wifi_app.h"

enum HsMenuIndex {
    HsMenuIndexSelectTarget,
    HsMenuIndexByChannel,
};

static void hs_menu_callback(void* context, uint32_t index) {
    WifiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wifi_app_scene_handshake_menu_on_enter(void* context) {
    WifiApp* app = context;

    submenu_add_item(app->submenu, "Select Target", HsMenuIndexSelectTarget, hs_menu_callback, app);
    submenu_add_item(app->submenu, "By Channel", HsMenuIndexByChannel, hs_menu_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewSubmenu);
}

bool wifi_app_scene_handshake_menu_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case HsMenuIndexSelectTarget:
            app->scanner_next_scene = WifiAppSceneHandshake;
            scene_manager_next_scene(app->scene_manager, WifiAppSceneScanner);
            consumed = true;
            break;
        case HsMenuIndexByChannel:
            scene_manager_next_scene(app->scene_manager, WifiAppSceneHandshakeChannel);
            consumed = true;
            break;
        }
    }

    return consumed;
}

void wifi_app_scene_handshake_menu_on_exit(void* context) {
    WifiApp* app = context;
    submenu_reset(app->submenu);
}
