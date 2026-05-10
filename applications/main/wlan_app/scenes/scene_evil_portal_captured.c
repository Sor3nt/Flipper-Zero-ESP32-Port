#include "../wlan_app.h"

static void ep_captured_back_cb(void* ctx) {
    WlanApp* app = ctx;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, WlanAppCustomEventEvilPortalCapturedBack);
}

void wlan_app_scene_evil_portal_captured_on_enter(void* context) {
    WlanApp* app = context;

    wlan_evil_portal_captured_view_set_data(
        app->evil_portal_captured_view_obj,
        app->evil_portal_valid_ssid,
        app->evil_portal_valid_pwd);
    wlan_evil_portal_captured_view_set_back_callback(
        app->evil_portal_captured_view_obj, ep_captured_back_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewEvilPortalCaptured);
}

bool wlan_app_scene_evil_portal_captured_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == WlanAppCustomEventEvilPortalCapturedBack) {
        // Captured + Run-Scene gemeinsam zurück bis zum Settings-Menu.
        scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, WlanAppSceneEvilPortalMenu);
        return true;
    }
    return false;
}

void wlan_app_scene_evil_portal_captured_on_exit(void* context) {
    WlanApp* app = context;
    wlan_evil_portal_captured_view_set_back_callback(
        app->evil_portal_captured_view_obj, NULL, NULL);
}
