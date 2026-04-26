#include "../wifi_app.h"
#include "../wifi_hal.h"

enum SubmenuIndex {
    SubmenuIndexSelect,
    SubmenuIndexSsidAttack,
    SubmenuIndexChannelAttack,
    SubmenuIndexSpamSSIDs,
    SubmenuIndexEvilPortal,
    SubmenuIndexConnect,
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

    bool connected = wifi_hal_is_connected();
    bool has_target = connected || app->ap_selected;

    if(has_target) {
        char label[48];
        snprintf(label, sizeof(label), "%s Attack", app->connected_ap.ssid);
        submenu_add_item(app->submenu, label, SubmenuIndexSsidAttack, wifi_app_scene_menu_submenu_callback, app);
        submenu_add_item(app->submenu, "Channel Attack", SubmenuIndexChannelAttack, wifi_app_scene_menu_submenu_callback, app);
        submenu_add_item(app->submenu, "Spam SSIDs", SubmenuIndexSpamSSIDs, wifi_app_scene_menu_submenu_callback, app);
        submenu_add_item(app->submenu, "Evil Portal", SubmenuIndexEvilPortal, wifi_app_scene_menu_submenu_callback, app);
        if(connected) {
            submenu_add_item(app->submenu, "Disconnect", SubmenuIndexDisconnect, wifi_app_scene_menu_submenu_callback, app);
        } else {
            submenu_add_item(app->submenu, "Connect", SubmenuIndexConnect, wifi_app_scene_menu_submenu_callback, app);
        }
        submenu_add_item(app->submenu, "Select Other WiFi", SubmenuIndexSelect, wifi_app_scene_menu_submenu_callback, app);
    } else {
        submenu_add_item(app->submenu, "Select WiFi", SubmenuIndexSelect, wifi_app_scene_menu_submenu_callback, app);
        submenu_add_item(app->submenu, "Channel Attack", SubmenuIndexChannelAttack, wifi_app_scene_menu_submenu_callback, app);
        submenu_add_item(app->submenu, "Spam SSIDs", SubmenuIndexSpamSSIDs, wifi_app_scene_menu_submenu_callback, app);
        submenu_add_item(app->submenu, "Evil Portal", SubmenuIndexEvilPortal, wifi_app_scene_menu_submenu_callback, app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewSubmenu);
}

bool wifi_app_scene_menu_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case SubmenuIndexSelect:
            app->scanner_next_scene = WifiAppSceneApDetail;
            scene_manager_next_scene(app->scene_manager, WifiAppSceneScanner);
            consumed = true;
            break;
        case SubmenuIndexConnect:
            if(app->connected_ap.is_open || app->connected_ap.has_password) {
                scene_manager_next_scene(app->scene_manager, WifiAppSceneConnect);
            } else {
                scene_manager_next_scene(app->scene_manager, WifiAppScenePasswordInput);
            }
            consumed = true;
            break;
        case SubmenuIndexSsidAttack:
            scene_manager_next_scene(app->scene_manager, WifiAppSceneSsidAttackMenu);
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
        case SubmenuIndexEvilPortal:
            scene_manager_next_scene(app->scene_manager, WifiAppSceneEvilPortalMenu);
            consumed = true;
            break;
        case SubmenuIndexDisconnect:
            wifi_hal_disconnect();
            wifi_hal_stop();
            wifi_app_scene_menu_on_exit(app);
            wifi_app_scene_menu_on_enter(app);
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