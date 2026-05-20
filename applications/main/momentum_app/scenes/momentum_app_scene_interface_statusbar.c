#include "../momentum_app.h"
#include "momentum_app_scene.h"
#include <momentum/momentum_settings.h>

static const char* const battery_names[] = {
    "Off",
    "Bar",
    "%",
    "Inv. %",
    "Retro 3",
    "Retro 5",
    "Bar %",
};

static void battery_icon_callback(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    momentum_settings.battery_icon = index;
    variable_item_set_current_value_text(item, battery_names[index]);
}

static void status_icons_callback(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    bool value = variable_item_get_current_value_index(item);
    momentum_settings.status_icons = value;
    variable_item_set_current_value_text(item, value ? "ON" : "OFF");
}

static void bar_borders_callback(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    bool value = variable_item_get_current_value_index(item);
    momentum_settings.bar_borders = value;
    variable_item_set_current_value_text(item, value ? "ON" : "OFF");
}

static void midnight_format_callback(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    bool value = variable_item_get_current_value_index(item);
    momentum_settings.midnight_format_00 = value;
    variable_item_set_current_value_text(item, value ? "00:00" : "12:00 AM");
}

void momentum_scene_interface_statusbar_on_enter(void* context) {
    MomentumApp* app = context;
    VariableItemList* var_item_list = app->var_item_list;
    VariableItem* item;

    variable_item_list_reset(var_item_list);

    item = variable_item_list_add(
        var_item_list, "Battery Icon", BatteryIconCount, battery_icon_callback, app);
    variable_item_set_current_value_index(item, momentum_settings.battery_icon);
    variable_item_set_current_value_text(item, battery_names[momentum_settings.battery_icon]);

    item = variable_item_list_add(
        var_item_list, "Status Icons", 2, status_icons_callback, app);
    variable_item_set_current_value_index(item, momentum_settings.status_icons);
    variable_item_set_current_value_text(item, momentum_settings.status_icons ? "ON" : "OFF");

    item = variable_item_list_add(
        var_item_list, "Bar Borders", 2, bar_borders_callback, app);
    variable_item_set_current_value_index(item, momentum_settings.bar_borders);
    variable_item_set_current_value_text(item, momentum_settings.bar_borders ? "ON" : "OFF");

    item = variable_item_list_add(
        var_item_list, "Clock Format", 2, midnight_format_callback, app);
    variable_item_set_current_value_index(item, momentum_settings.midnight_format_00);
    variable_item_set_current_value_text(
        item, momentum_settings.midnight_format_00 ? "00:00" : "12:00 AM");

    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumAppViewVarItemList);
}

bool momentum_scene_interface_statusbar_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void momentum_scene_interface_statusbar_on_exit(void* context) {
    MomentumApp* app = context;
    variable_item_list_reset(app->var_item_list);
    momentum_settings_save();
}
