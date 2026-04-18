#include "../wifi_app.h"

static void wifi_app_custom_ssid_callback(void* context) {
    WifiApp* app = context;
    strncpy(app->single_ssid, app->password_input, sizeof(app->single_ssid) - 1);
    app->beacon_mode = WifiAppBeaconModeCustom;
    scene_manager_next_scene(app->scene_manager, WifiAppSceneBeaconSpam);
}

void wifi_app_scene_custom_ssid_on_enter(void* context) {
    WifiApp* app = context;
    text_input_reset(app->text_input);
    text_input_set_header_text(app->text_input, "Enter Base SSID (max 32 chars)");
    text_input_set_result_callback(app->text_input, wifi_app_custom_ssid_callback, app, app->password_input, sizeof(app->password_input), true);
    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewTextInput);
}

bool wifi_app_scene_custom_ssid_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void wifi_app_scene_custom_ssid_on_exit(void* context) {
    WifiApp* app = context;
    memset(app->password_input, 0, sizeof(app->password_input));
}