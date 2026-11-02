#include "../gpio_app_i.h"
#include "../gpio_infrared_settings.h"

#include <boards/board.h>
#include <furi_hal_power.h>

static const char* const gpio_vbus_state_text[] = {
    "OFF",
    "ON",
};

static void gpio_vbus_state_changed(VariableItem* item) {
    GpioApp* app = variable_item_get_context(item);
    const bool enabled = variable_item_get_current_value_index(item) != 0;
    variable_item_set_current_value_text(item, gpio_vbus_state_text[enabled ? 1 : 0]);
    app->infrared_settings.otg_enabled = enabled;
#if defined(BOARD_IR_TX_REQUIRES_VBUS) && BOARD_IR_TX_REQUIRES_VBUS
    power_enable_otg(
        app->power, enabled && furi_hal_power_is_running_on_battery());
#else
    power_enable_otg(app->power, enabled);
#endif
    gpio_infrared_settings_save(&app->infrared_settings);
}

void gpio_scene_vbus_settings_on_enter(void* context) {
    GpioApp* app = context;
    VariableItemList* list = app->var_item_list;

    /* The IR signal output moved to Infrared > Infrared Settings, as on the original. */
    VariableItem* item = variable_item_list_add(
        list, "VBUS 5V", COUNT_OF(gpio_vbus_state_text), gpio_vbus_state_changed, app);
    const uint8_t vbus_index = app->infrared_settings.otg_enabled ? 1 : 0;
    variable_item_set_current_value_index(item, vbus_index);
    variable_item_set_current_value_text(item, gpio_vbus_state_text[vbus_index]);

    view_dispatcher_switch_to_view(app->view_dispatcher, GpioAppViewVarItemList);
}

bool gpio_scene_vbus_settings_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    return event.type == SceneManagerEventTypeCustom;
}

void gpio_scene_vbus_settings_on_exit(void* context) {
    GpioApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
