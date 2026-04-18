#include "../wifi_app.h"

enum SpamSsidsIndex {
    SpamSsidsIndexFunny,
    SpamSsidsIndexRickroll,
    SpamSsidsIndexRandom,
    SpamSsidsIndexCustom,
};

static void spam_ssids_callback(void* context, uint32_t index) {
    WifiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wifi_app_scene_spam_ssids_menu_on_enter(void* context) {
    WifiApp* app = context;
    submenu_add_item(app->submenu, "Funny SSIDs", SpamSsidsIndexFunny, spam_ssids_callback, app);
    submenu_add_item(app->submenu, "Rick Roll", SpamSsidsIndexRickroll, spam_ssids_callback, app);
    submenu_add_item(app->submenu, "Random SSIDs", SpamSsidsIndexRandom, spam_ssids_callback, app);
    submenu_add_item(app->submenu, "Custom SSIDs", SpamSsidsIndexCustom, spam_ssids_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewSubmenu);
}

bool wifi_app_scene_spam_ssids_menu_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case SpamSsidsIndexFunny:
            app->beacon_mode = WifiAppBeaconModeFunny;
            scene_manager_next_scene(app->scene_manager, WifiAppSceneBeaconSpam);
            consumed = true;
            break;
        case SpamSsidsIndexRickroll:
            app->beacon_mode = WifiAppBeaconModeRickroll;
            scene_manager_next_scene(app->scene_manager, WifiAppSceneBeaconSpam);
            consumed = true;
            break;
        case SpamSsidsIndexRandom:
            app->beacon_mode = WifiAppBeaconModeRandom;
            scene_manager_next_scene(app->scene_manager, WifiAppSceneBeaconSpam);
            consumed = true;
            break;
        case SpamSsidsIndexCustom:
            scene_manager_next_scene(app->scene_manager, WifiAppSceneCustomSsid);
            consumed = true;
            break;
        }
    }
    return consumed;
}

void wifi_app_scene_spam_ssids_menu_on_exit(void* context) {
    WifiApp* app = context;
    submenu_reset(app->submenu);
}