#include "../momentum_app.h"
#include "momentum_app_scene.h"
#include <momentum/momentum_settings.h>

static void momentum_scene_misc_screen_dark_mode_callback(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    bool value = variable_item_get_current_value_index(item);
    momentum_settings.dark_mode = value;
    variable_item_set_current_value_text(item, value ? "ON" : "OFF");
}

static void build_screen_menu(MomentumApp* app) {
    VariableItemList* var_item_list = app->var_item_list;
    VariableItem* item;

    variable_item_list_reset(var_item_list);

    item = variable_item_list_add(
        var_item_list, "Dark Mode", 2, momentum_scene_misc_screen_dark_mode_callback, app);
    variable_item_set_current_value_index(item, momentum_settings.dark_mode);
    variable_item_set_current_value_text(item, momentum_settings.dark_mode ? "ON" : "OFF");
}

void momentum_scene_misc_screen_on_enter(void* context) {
    MomentumApp* app = context;
    VariableItemList* var_item_list = app->var_item_list;

    variable_item_list_set_enter_callback(var_item_list, NULL, NULL);
    build_screen_menu(app);

    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumAppViewVarItemList);
}

bool momentum_scene_misc_screen_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void momentum_scene_misc_screen_on_exit(void* context) {
    MomentumApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
