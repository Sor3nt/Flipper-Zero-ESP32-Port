#include "../wlan_app.h"

static void mitm_code_cb(void* context) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, WlanAppCustomEventMitmInjectCodeEntered);
}

void wlan_app_scene_mitm_inject_code_on_enter(void* context) {
    WlanApp* app = context;

    text_input_set_header_text(app->text_input, "Inject Code:");
    text_input_set_result_callback(
        app->text_input,
        mitm_code_cb,
        app,
        app->mitm_inject_code,
        sizeof(app->mitm_inject_code),
        false);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewTextInput);
}

bool wlan_app_scene_mitm_inject_code_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventMitmInjectCodeEntered) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    }

    return consumed;
}

void wlan_app_scene_mitm_inject_code_on_exit(void* context) {
    WlanApp* app = context;
    text_input_reset(app->text_input);
}
