#include "../wlan_app.h"
#include "../wlan_hal.h"

#define SSID_SPAM_CUSTOM_DONE 1

static void ssid_spam_custom_cb(void* context) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, SSID_SPAM_CUSTOM_DONE);
}

void wlan_app_scene_ssid_spam_custom_on_enter(void* context) {
    WlanApp* app = context;
    text_input_set_header_text(app->text_input, "Base SSID:");
    text_input_set_result_callback(
        app->text_input,
        ssid_spam_custom_cb,
        app,
        app->beacon_custom_ssid,
        sizeof(app->beacon_custom_ssid),
        false);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewTextInput);
}

bool wlan_app_scene_ssid_spam_custom_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom &&
       event.event == SSID_SPAM_CUSTOM_DONE) {
        app->beacon_mode = WlanHalBeaconModeCustom;
        scene_manager_next_scene(app->scene_manager, WlanAppSceneSsidSpamRun);
        consumed = true;
    }
    return consumed;
}

void wlan_app_scene_ssid_spam_custom_on_exit(void* context) {
    WlanApp* app = context;
    text_input_reset(app->text_input);
}
