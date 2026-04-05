#include "../wifi_app.h"

static void password_input_callback(void* context) {
    WifiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WifiAppCustomEventPasswordEntered);
}

void wifi_app_scene_password_input_on_enter(void* context) {
    WifiApp* app = context;

    app->password_input[0] = '\0';

    text_input_set_header_text(app->text_input, "WiFi Password:");
    text_input_set_result_callback(
        app->text_input,
        password_input_callback,
        app,
        app->password_input,
        sizeof(app->password_input),
        false);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewTextInput);
}

bool wifi_app_scene_password_input_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WifiAppCustomEventPasswordEntered) {
            scene_manager_next_scene(app->scene_manager, WifiAppSceneConnect);
            consumed = true;
        }
    }

    return consumed;
}

void wifi_app_scene_password_input_on_exit(void* context) {
    WifiApp* app = context;
    text_input_reset(app->text_input);
}
