#include "../nrf24_app.h"

enum {
    JammerMenuIndexWifi = 1,
    JammerMenuIndexChannel,
    JammerMenuIndexSmart,
};

static void jammer_menu_callback(void* context, uint32_t index) {
    Nrf24App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void nrf24_app_scene_jammer_menu_on_enter(void* context) {
    Nrf24App* app = context;

    submenu_set_header(app->submenu, "Jammer");
    submenu_add_item(app->submenu, "WiFi", JammerMenuIndexWifi, jammer_menu_callback, app);
    submenu_add_item(
        app->submenu, "by Channel", JammerMenuIndexChannel, jammer_menu_callback, app);
    submenu_add_item(app->submenu, "Smart", JammerMenuIndexSmart, jammer_menu_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewSubmenu);
}

bool nrf24_app_scene_jammer_menu_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(event.event) {
    case JammerMenuIndexWifi:
        scene_manager_next_scene(app->scene_manager, Nrf24AppSceneWifiScan);
        return true;
    case JammerMenuIndexChannel:
        scene_manager_next_scene(app->scene_manager, Nrf24AppSceneChJammer);
        return true;
    case JammerMenuIndexSmart:
        scene_manager_next_scene(app->scene_manager, Nrf24AppSceneSmartJam);
        return true;
    default:
        return false;
    }
}

void nrf24_app_scene_jammer_menu_on_exit(void* context) {
    Nrf24App* app = context;
    submenu_reset(app->submenu);
}
