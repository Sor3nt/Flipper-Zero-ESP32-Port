#include "../momentum_app.h"

enum VarItemListIndex {
    VarItemListIndexDarkMode,
    VarItemListIndexLeftHanded,
};

void momentum_app_scene_misc_screen_var_item_list_callback(void* context, uint32_t index) {
    MomentumApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void momentum_app_scene_misc_screen_dark_mode_changed(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    bool value = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, value ? "ON" : "OFF");
    momentum_settings.dark_mode = value;
    app->save_settings = true;
}

static void momentum_app_scene_misc_screen_hand_orient_changed(VariableItem* item) {
    bool value = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, value ? "ON" : "OFF");
    if(value) {
        furi_hal_rtc_set_flag(FuriHalRtcFlagHandOrient);
    } else {
        furi_hal_rtc_reset_flag(FuriHalRtcFlagHandOrient);
    }
}

void momentum_app_scene_misc_screen_on_enter(void* context) {
    MomentumApp* app = context;
    VariableItemList* var_item_list = app->var_item_list;
    VariableItem* item;
    uint8_t value_index;

    item = variable_item_list_add(
        var_item_list, "Dark Mode", 2, momentum_app_scene_misc_screen_dark_mode_changed, app);
    variable_item_set_current_value_index(item, momentum_settings.dark_mode);
    variable_item_set_current_value_text(item, momentum_settings.dark_mode ? "ON" : "OFF");

    item = variable_item_list_add(
        var_item_list, "Left Handed", 2, momentum_app_scene_misc_screen_hand_orient_changed, app);
    value_index = furi_hal_rtc_is_flag_set(FuriHalRtcFlagHandOrient);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, value_index ? "ON" : "OFF");

    variable_item_list_set_enter_callback(
        var_item_list, momentum_app_scene_misc_screen_var_item_list_callback, app);

    variable_item_list_set_selected_item(
        var_item_list,
        scene_manager_get_scene_state(app->scene_manager, MomentumAppSceneMiscScreen));

    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumAppViewVarItemList);
}

bool momentum_app_scene_misc_screen_on_event(void* context, SceneManagerEvent event) {
    MomentumApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(app->scene_manager, MomentumAppSceneMiscScreen, event.event);
        consumed = true;
    }

    return consumed;
}

void momentum_app_scene_misc_screen_on_exit(void* context) {
    MomentumApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
