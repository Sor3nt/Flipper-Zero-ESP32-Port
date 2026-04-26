#include "../wifi_app.h"
#include "../views/evil_portal_captured_view.h"

static void evil_portal_captured_back_cb(void* ctx) {
    WifiApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, WifiAppCustomEventEvilPortalStop);
}

void wifi_app_scene_evil_portal_captured_on_enter(void* context) {
    WifiApp* app = context;

    evil_portal_captured_view_set_data(
        app->evil_portal_captured_view_obj,
        app->evil_portal_valid_ssid,
        app->evil_portal_valid_pwd);
    evil_portal_captured_view_set_back_callback(
        app->evil_portal_captured_view_obj, evil_portal_captured_back_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewEvilPortalCaptured);
}

bool wifi_app_scene_evil_portal_captured_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == WifiAppCustomEventEvilPortalStop) {
        // Pop both captured + run scenes back to the menu
        scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, WifiAppSceneEvilPortalMenu);
        return true;
    }
    return false;
}

void wifi_app_scene_evil_portal_captured_on_exit(void* context) {
    WifiApp* app = context;
    evil_portal_captured_view_set_back_callback(app->evil_portal_captured_view_obj, NULL, NULL);
}
