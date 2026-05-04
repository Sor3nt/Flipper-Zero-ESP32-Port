#include "../ble_spam_app.h"

enum TextInputResult {
    TextInputResultOk,
};

static void ble_spam_scene_pair_spam_custom_text_input_callback(void* context) {
    BleSpamApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, TextInputResultOk);
}

void ble_spam_scene_pair_spam_custom_on_enter(void* context) {
    BleSpamApp* app = context;
    TextInput* text_input = app->text_input;

    text_input_set_header_text(text_input, "Custom pairable device name");
    text_input_set_result_callback(
        text_input,
        ble_spam_scene_pair_spam_custom_text_input_callback,
        app,
        app->custom_pair_name,
        sizeof(app->custom_pair_name),
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewTextInput);
}

bool ble_spam_scene_pair_spam_custom_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom && event.event == TextInputResultOk) {
        if(app->custom_pair_name[0] == '\0') {
            strlcpy(app->custom_pair_name, "Pairable Device", sizeof(app->custom_pair_name));
        }
        scene_manager_next_scene(app->scene_manager, BleSpamSceneRunning);
        consumed = true;
    }

    return consumed;
}

void ble_spam_scene_pair_spam_custom_on_exit(void* context) {
    BleSpamApp* app = context;
    text_input_reset(app->text_input);
}
