#include "../wifi_app.h"
#include "../wifi_hal.h"

enum SubmenuIndex {
    SubmenuIndexConnect,
    SubmenuIndexSsidAttack,
    SubmenuIndexChannelAttack,
    SubmenuIndexSpamSSIDs,
    SubmenuIndexDisconnect,
};

static void wifi_app_scene_menu_submenu_callback(void* context, uint32_t index) {
    WifiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wifi_app_scene_menu_on_enter(void* context) {
    WifiApp* app = context;

    if(wifi_hal_is_started() && !wifi_hal_is_connected()) {
        wifi_hal_stop();
    }

    if(wifi_hal_is_connected()) {
        char label[48];
        snprintf(label, sizeof(label), "%s Attack", app->connected_ap.ssid);
        submenu_add_item(app->submenu, label, SubmenuIndexSsidAttack, wifi_app_scene_menu_submenu_callback, app);
        submenu_add_item(app->submenu, "Channel Attack", SubmenuIndexChannelAttack, wifi_app_scene_menu_submenu_callback, app);
        submenu_add_item(app->submenu, "Spam SSIDs", SubmenuIndexSpamSSIDs, wifi_app_scene_menu_submenu_callback, app);
        submenu_add_item(app->submenu, "Disconnect", SubmenuIndexDisconnect, wifi_app_scene_menu_submenu_callback, app);
    } else {
        submenu_add_item(app->submenu, "Connect to WiFi", SubmenuIndexConnect, wifi_app_scene_menu_submenu_callback, app);
        submenu_add_item(app->submenu, "Channel Attack", SubmenuIndexChannelAttack, wifi_app_scene_menu_submenu_callback, app);
        submenu_add_item(app->submenu, "Spam SSIDs", SubmenuIndexSpamSSIDs, wifi_app_scene_menu_submenu_callback, app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewSubmenu);
}

bool wifi_app_scene_menu_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case SubmenuIndexConnect:
            app->scanner_next_scene = WifiAppSceneConnect;
            scene_manager_next_scene(app->scene_manager, WifiAppSceneScanner);
            consumed = true;
            break;
        case SubmenuIndexSsidAttack:
            scene_manager_next_scene(app->scene_manager, WifiAppSceneSpamSSIDsMenu);
            consumed = true;
            break;
        case SubmenuIndexChannelAttack:
            scene_manager_next_scene(app->scene_manager, WifiAppSceneChannelAttackMenu);
            consumed = true;
            break;
        case SubmenuIndexSpamSSIDs:
            scene_manager_next_scene(app->scene_manager, WifiAppSceneSpamSSIDsMenu);
            consumed = true;
            break;
        case SubmenuIndexDisconnect:
            wifi_hal_disconnect();
            wifi_hal_stop();
            scene_manager_search_and_switch_to_previous_scene(app->scene_manager, WifiAppSceneMenu);
            consumed = true;
            break;
        }
    }

    return consumed;
}

void wifi_app_scene_menu_on_exit(void* context) {
    WifiApp* app = context;
    submenu_reset(app->submenu);
}