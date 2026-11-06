/* Web-Filesystem entry menu (shown when not connected): choose Select Wifi (STA)
 * or Dedicated AP. Only reached when no WiFi connection is active; a live
 * connection goes straight to the info scene. */

#include "../wlan_app.h"

enum {
    WebFsMenuSelectWifi,
    WebFsMenuDedicatedAp,
};

static void webfs_menu_submenu_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher,
        index == WebFsMenuSelectWifi ? WlanAppCustomEventWebFsSelectWifi :
                                       WlanAppCustomEventWebFsDedicatedAp);
}

void wlan_app_scene_webfs_menu_on_enter(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Web-Filesystem");
    submenu_add_item(
        app->submenu, "Select Wifi", WebFsMenuSelectWifi, webfs_menu_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Dedicated AP", WebFsMenuDedicatedAp, webfs_menu_submenu_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSubmenu);
}

bool wlan_app_scene_webfs_menu_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventWebFsSelectWifi) {
            // Reuse the Select Wifi / connect flow; routes to the info scene on
            // successful connect (see scene_ssid_connect).
            app->webfs_flow = true;
            scene_manager_next_scene(app->scene_manager, WlanAppSceneConnect);
            consumed = true;
        } else if(event.event == WlanAppCustomEventWebFsDedicatedAp) {
            scene_manager_next_scene(app->scene_manager, WlanAppSceneWebFsAp);
            consumed = true;
        }
    }
    return consumed;
}

void wlan_app_scene_webfs_menu_on_exit(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
}
