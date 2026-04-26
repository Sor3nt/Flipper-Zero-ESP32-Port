#include "../wifi_app.h"

static void evil_portal_ssid_cb(void* context) {
    WifiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WifiAppCustomEventEvilPortalSsidEntered);
}

void wifi_app_scene_evil_portal_ssid_on_enter(void* context) {
    WifiApp* app = context;

    text_input_set_header_text(app->text_input, "Fake SSID:");
    text_input_set_result_callback(
        app->text_input,
        evil_portal_ssid_cb,
        app,
        app->evil_portal_ssid,
        sizeof(app->evil_portal_ssid),
        false);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewTextInput);
}

bool wifi_app_scene_evil_portal_ssid_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WifiAppCustomEventEvilPortalSsidEntered) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    }

    return consumed;
}

void wifi_app_scene_evil_portal_ssid_on_exit(void* context) {
    WifiApp* app = context;
    text_input_reset(app->text_input);
}
