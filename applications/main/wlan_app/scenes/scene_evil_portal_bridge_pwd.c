#include "../wlan_app.h"

static void ep_bridge_pwd_cb(void* context) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, WlanAppCustomEventEvilPortalBridgePwdEntered);
}

void wlan_app_scene_evil_portal_bridge_pwd_on_enter(void* context) {
    WlanApp* app = context;

    text_input_set_header_text(app->text_input, "Bridge WiFi Password:");
    text_input_set_result_callback(
        app->text_input,
        ep_bridge_pwd_cb,
        app,
        app->evil_portal_bridge_password,
        sizeof(app->evil_portal_bridge_password),
        false);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewTextInput);
}

bool wlan_app_scene_evil_portal_bridge_pwd_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventEvilPortalBridgePwdEntered) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    }

    return consumed;
}

void wlan_app_scene_evil_portal_bridge_pwd_on_exit(void* context) {
    WlanApp* app = context;
    text_input_reset(app->text_input);
}
