#include "../wlan_app.h"

static void ep_ssid_cb(void* context) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, WlanAppCustomEventEvilPortalSsidEntered);
}

void wlan_app_scene_evil_portal_ssid_on_enter(void* context) {
    WlanApp* app = context;

    text_input_set_header_text(app->text_input, "Fake SSID:");
    text_input_set_result_callback(
        app->text_input,
        ep_ssid_cb,
        app,
        app->evil_portal_ssid,
        sizeof(app->evil_portal_ssid),
        false);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewTextInput);
}

bool wlan_app_scene_evil_portal_ssid_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventEvilPortalSsidEntered) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    }

    return consumed;
}

void wlan_app_scene_evil_portal_ssid_on_exit(void* context) {
    WlanApp* app = context;
    text_input_reset(app->text_input);
}
