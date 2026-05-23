#include "../momentum_app.h"
#include "momentum_app_scene.h"
#include <momentum/momentum_settings.h>

static const char* const browser_path_names[] = {
    "Off",
    "Current",
    "Brief",
    "Full",
};

static void sort_dirs_first_callback(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    bool value = variable_item_get_current_value_index(item);
    momentum_settings.sort_dirs_first = value;
    variable_item_set_current_value_text(item, value ? "ON" : "OFF");
}

static void show_hidden_callback(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    bool value = variable_item_get_current_value_index(item);
    momentum_settings.show_hidden_files = value;
    variable_item_set_current_value_text(item, value ? "ON" : "OFF");
}

static void path_mode_callback(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    momentum_settings.browser_path_mode = index;
    variable_item_set_current_value_text(item, browser_path_names[index]);
}

void momentum_scene_interface_filebrowser_on_enter(void* context) {
    MomentumApp* app = context;
    VariableItemList* var_item_list = app->var_item_list;
    VariableItem* item;

    variable_item_list_reset(var_item_list);

    item = variable_item_list_add(
        var_item_list, "Dirs First", 2, sort_dirs_first_callback, app);
    variable_item_set_current_value_index(item, momentum_settings.sort_dirs_first);
    variable_item_set_current_value_text(
        item, momentum_settings.sort_dirs_first ? "ON" : "OFF");

    item = variable_item_list_add(
        var_item_list, "Show Hidden", 2, show_hidden_callback, app);
    variable_item_set_current_value_index(item, momentum_settings.show_hidden_files);
    variable_item_set_current_value_text(
        item, momentum_settings.show_hidden_files ? "ON" : "OFF");

    item = variable_item_list_add(
        var_item_list, "Path Mode", BrowserPathCount, path_mode_callback, app);
    variable_item_set_current_value_index(item, momentum_settings.browser_path_mode);
    variable_item_set_current_value_text(
        item, browser_path_names[momentum_settings.browser_path_mode]);

    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumAppViewVarItemList);
}

bool momentum_scene_interface_filebrowser_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void momentum_scene_interface_filebrowser_on_exit(void* context) {
    MomentumApp* app = context;
    variable_item_list_reset(app->var_item_list);
    momentum_settings_save();
}
