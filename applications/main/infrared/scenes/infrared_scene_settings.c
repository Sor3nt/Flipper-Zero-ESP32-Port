#include "../infrared_app_i.h"

#include <boards/board.h>

#define INFRARED_SETTINGS_STR_(x) #x
#define INFRARED_SETTINGS_STR(x)  INFRARED_SETTINGS_STR_(x)

static const char* const infrared_scene_settings_tx_output_text[] = {
    "Internal",
    "External",
};

static void infrared_scene_settings_tx_output_changed(VariableItem* item) {
    InfraredApp* infrared = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, infrared_scene_settings_tx_output_text[index]);
    infrared_set_tx_pin(
        infrared, index ? FuriHalInfraredTxPinExtPA7 : FuriHalInfraredTxPinInternal);
}

void infrared_scene_settings_on_enter(void* context) {
    InfraredApp* infrared = context;
    VariableItemList* var_item_list = infrared->var_item_list;

    VariableItem* item = variable_item_list_add(
        var_item_list,
        "TX Output",
        COUNT_OF(infrared_scene_settings_tx_output_text),
        infrared_scene_settings_tx_output_changed,
        infrared);
    /* Older settings may hold "Detect", which resolves to internal on this port. */
    const uint8_t index = infrared->app_state.tx_pin == FuriHalInfraredTxPinExtPA7 ? 1 : 0;
    variable_item_set_current_value_index(item, index);
    variable_item_set_current_value_text(item, infrared_scene_settings_tx_output_text[index]);

#if defined(BOARD_PIN_IR_TX_EXT)
    item = variable_item_list_add(var_item_list, "External Pin", 1, NULL, NULL);
    variable_item_set_current_value_text(item, "GPIO" INFRARED_SETTINGS_STR(BOARD_PIN_IR_TX_EXT));
#endif

    view_dispatcher_switch_to_view(infrared->view_dispatcher, InfraredViewVariableList);
}

bool infrared_scene_settings_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void infrared_scene_settings_on_exit(void* context) {
    InfraredApp* infrared = context;
    variable_item_list_reset(infrared->var_item_list);
    infrared_save_settings(infrared);
}
