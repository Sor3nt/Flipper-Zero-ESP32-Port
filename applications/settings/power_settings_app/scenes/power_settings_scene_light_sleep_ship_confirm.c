#include "../power_settings_app.h"

static void power_settings_scene_light_sleep_ship_confirm_callback(
    DialogExResult result,
    void* context) {
    furi_assert(context);
    PowerSettingsApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, result);
}

void power_settings_scene_light_sleep_ship_confirm_on_enter(void* context) {
    PowerSettingsApp* app = context;
    DialogEx* dialog = app->dialog;

    dialog_ex_set_header(dialog, "Auto Light Sleep Needed", 64, 2, AlignCenter, AlignTop);
    dialog_ex_set_text(
        dialog, "Needed for Ship Mode.\nEnable Auto Light Sleep?", 64, 18, AlignCenter, AlignTop);
    dialog_ex_set_left_button_text(dialog, "Back");
    dialog_ex_set_right_button_text(dialog, "Select");
    dialog_ex_set_result_callback(dialog, power_settings_scene_light_sleep_ship_confirm_callback);
    dialog_ex_set_context(dialog, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, PowerSettingsAppViewDialog);
}

bool power_settings_scene_light_sleep_ship_confirm_on_event(void* context, SceneManagerEvent event) {
    PowerSettingsApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == DialogExResultRight) {
        app->settings.light_sleep_enabled = true;
        app->settings.light_sleep_ship_mode_enabled = true;
    }

    if(!scene_manager_previous_scene(app->scene_manager)) {
        scene_manager_next_scene(app->scene_manager, PowerSettingsAppSceneStart);
    }
    return true;
}

void power_settings_scene_light_sleep_ship_confirm_on_exit(void* context) {
    PowerSettingsApp* app = context;
    dialog_ex_reset(app->dialog);
}