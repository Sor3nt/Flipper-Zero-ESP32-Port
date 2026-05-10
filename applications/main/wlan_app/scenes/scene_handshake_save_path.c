#include "../wlan_app.h"

static char s_save_path_buf[WLAN_HS_SAVE_PATH_MAX];

static void hs_save_path_cb(void* context) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, WlanAppCustomEventHandshakeSavePathEntered);
}

void wlan_app_scene_handshake_save_path_on_enter(void* context) {
    WlanApp* app = context;
    text_input_reset(app->text_input);

    // Aktuelles Setting als Default in den Buffer.
    memset(s_save_path_buf, 0, sizeof(s_save_path_buf));
    strncpy(s_save_path_buf, app->hs_settings.save_path, sizeof(s_save_path_buf) - 1);

    text_input_set_header_text(app->text_input, "Save folder:");
    text_input_set_result_callback(
        app->text_input,
        hs_save_path_cb,
        app,
        s_save_path_buf,
        sizeof(s_save_path_buf),
        false);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewTextInput);
}

bool wlan_app_scene_handshake_save_path_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom &&
       event.event == WlanAppCustomEventHandshakeSavePathEntered) {
        // Whitespace-/Slash-Trim am Ende, leerer String → Default zurücksetzen.
        size_t len = strlen(s_save_path_buf);
        while(len > 0 && (s_save_path_buf[len - 1] == ' ' || s_save_path_buf[len - 1] == '/')) {
            s_save_path_buf[--len] = '\0';
        }
        const char* new_path = s_save_path_buf[0] ? s_save_path_buf : WLAN_HS_DEFAULT_SAVE_PATH;
        memset(app->hs_settings.save_path, 0, sizeof(app->hs_settings.save_path));
        strncpy(app->hs_settings.save_path, new_path, sizeof(app->hs_settings.save_path) - 1);
        wlan_handshake_settings_save(&app->hs_settings);
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }
    return consumed;
}

void wlan_app_scene_handshake_save_path_on_exit(void* context) {
    WlanApp* app = context;
    text_input_reset(app->text_input);
}
