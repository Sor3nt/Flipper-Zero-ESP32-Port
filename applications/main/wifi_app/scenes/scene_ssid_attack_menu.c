#include "../wifi_app.h"
#include "../wifi_hal.h"

enum SsidAttackIndex {
    SsidAttackIndexHandshake,
    SsidAttackIndexDeauth,
    SsidAttackIndexNetscan,
    SsidAttackIndexAirSnitch,
    SsidAttackIndexCrawler,
};

static void ssid_attack_callback(void* context, uint32_t index) {
    WifiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wifi_app_scene_ssid_attack_menu_on_enter(void* context) {
    WifiApp* app = context;
    bool connected = wifi_hal_is_connected();

    submenu_add_item(
        app->submenu, "Capture Handshake", SsidAttackIndexHandshake,
        ssid_attack_callback, app);
    submenu_add_item(
        app->submenu, "Deauth", SsidAttackIndexDeauth,
        ssid_attack_callback, app);
    if(connected) {
        submenu_add_item(
            app->submenu, "Network Scanner", SsidAttackIndexNetscan,
            ssid_attack_callback, app);
        submenu_add_item(
            app->submenu, "AirSnitch", SsidAttackIndexAirSnitch,
            ssid_attack_callback, app);
        submenu_add_item(
            app->submenu, "Crawler", SsidAttackIndexCrawler,
            ssid_attack_callback, app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewSubmenu);
}

bool wifi_app_scene_ssid_attack_menu_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case SsidAttackIndexHandshake:
            scene_manager_next_scene(app->scene_manager, WifiAppSceneHandshake);
            consumed = true;
            break;
        case SsidAttackIndexDeauth:
            app->deauth_mode = WifiAppDeauthModeSsid;
            scene_manager_next_scene(app->scene_manager, WifiAppSceneDeauther);
            consumed = true;
            break;
        case SsidAttackIndexNetscan:
            scene_manager_next_scene(app->scene_manager, WifiAppSceneNetscan);
            consumed = true;
            break;
        case SsidAttackIndexAirSnitch:
            scene_manager_next_scene(app->scene_manager, WifiAppSceneAirsnScan);
            consumed = true;
            break;
        case SsidAttackIndexCrawler:
            scene_manager_next_scene(app->scene_manager, WifiAppSceneCrawlerInput);
            consumed = true;
            break;
        }
    }

    return consumed;
}

void wifi_app_scene_ssid_attack_menu_on_exit(void* context) {
    WifiApp* app = context;
    submenu_reset(app->submenu);
}
