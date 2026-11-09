#include "../infrared_app_i.h"

#if !INFRARED_IS_ESP32S3
static const char* infrared_scene_gpio_settings_pin_text[] = {
    "Flipper",
    "2 (A7)",
    "Detect",
};
#endif

static const char* infrared_scene_gpio_settings_otg_text[] = {
    "OFF",
    "ON",
};

#if !INFRARED_IS_ESP32S3
static void infrared_scene_gpio_settings_pin_change_callback(VariableItem* item) {
    InfraredApp* infrared = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, infrared_scene_gpio_settings_pin_text[index]);
    view_dispatcher_send_custom_event(
        infrared->view_dispatcher,
        infrared_custom_event_pack(InfraredCustomEventTypeGpioTxPinChanged, index));
}
#endif

static void infrared_scene_gpio_settings_otg_change_callback(VariableItem* item) {
    InfraredApp* infrared = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, infrared_scene_gpio_settings_otg_text[index]);
    view_dispatcher_send_custom_event(
        infrared->view_dispatcher,
        infrared_custom_event_pack(InfraredCustomEventTypeGpioOtgChanged, index));
}

/* Boards with an external IR TX pin: "TX Pin: Internal / GPIOn". The pin is
 * taken only while a signal is being sent and released right after. */
static char infrared_scene_ext_pin_text[12];

static bool infrared_scene_gpio_settings_has_ext_pin(void) {
    return furi_hal_infrared_get_ext_tx_pin() != UINT16_MAX;
}

static void infrared_scene_gpio_settings_ext_pin_change_callback(VariableItem* item) {
    InfraredApp* infrared = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(
        item, index ? infrared_scene_ext_pin_text : "Internal");
    view_dispatcher_send_custom_event(
        infrared->view_dispatcher,
        infrared_custom_event_pack(InfraredCustomEventTypeGpioTxPinChanged, index));
}

static void infrared_scene_gpio_settings_init_ext(InfraredApp* infrared) {
    snprintf(
        infrared_scene_ext_pin_text,
        sizeof(infrared_scene_ext_pin_text),
        "GPIO%u",
        furi_hal_infrared_get_ext_tx_pin());

    VariableItem* item = variable_item_list_add(
        infrared->var_item_list,
        "TX Pin",
        2,
        infrared_scene_gpio_settings_ext_pin_change_callback,
        infrared);
    const uint8_t value_index =
        furi_hal_infrared_get_tx_output() == FuriHalInfraredTxPinExtPA7 ? 1 : 0;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(
        item, value_index ? infrared_scene_ext_pin_text : "Internal");
}

static void infrared_scene_gpio_settings_add_otg_item(
    InfraredApp* infrared,
    bool allow_auto) {
    VariableItem* item = variable_item_list_add(
        infrared->var_item_list,
#if INFRARED_IS_ESP32S3
        "VBUS 5V",
#else
        "5V on GPIO",
#endif
        COUNT_OF(infrared_scene_gpio_settings_otg_text),
        infrared_scene_gpio_settings_otg_change_callback,
        infrared);

    if(allow_auto && infrared->app_state.tx_pin >= FuriHalInfraredTxPinMax) {
        variable_item_set_values_count(item, 1);
        variable_item_set_current_value_index(item, 0);
        variable_item_set_current_value_text(item, "Auto");
    } else {
        const uint8_t value_index = infrared->app_state.is_otg_enabled;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(
            item, infrared_scene_gpio_settings_otg_text[value_index]);
    }
}

static void infrared_scene_gpio_settings_init(InfraredApp* infrared) {
#if INFRARED_IS_ESP32S3
    if(infrared_scene_gpio_settings_has_ext_pin()) {
        infrared_scene_gpio_settings_init_ext(infrared);
    } else {
        /* The S3 uses only its internal IR LED; there is no signal-output choice. */
        infrared->app_state.tx_pin = FuriHalInfraredTxPinInternal;
    }
    infrared_scene_gpio_settings_add_otg_item(infrared, false);
#else
    if(infrared_scene_gpio_settings_has_ext_pin()) {
        infrared_scene_gpio_settings_init_ext(infrared);
        return;
    }

    VariableItemList* var_item_list = infrared->var_item_list;
    VariableItem* item = variable_item_list_add(
        var_item_list,
        "Signal Output",
        COUNT_OF(infrared_scene_gpio_settings_pin_text),
        infrared_scene_gpio_settings_pin_change_callback,
        infrared);

    const uint8_t value_index = infrared->app_state.tx_pin;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(
        item, infrared_scene_gpio_settings_pin_text[value_index]);
    infrared_scene_gpio_settings_add_otg_item(infrared, true);
#endif
}

void infrared_scene_gpio_settings_on_enter(void* context) {
    InfraredApp* infrared = context;
    infrared_scene_gpio_settings_init(infrared);
    view_dispatcher_switch_to_view(infrared->view_dispatcher, InfraredViewVariableList);
}

bool infrared_scene_gpio_settings_on_event(void* context, SceneManagerEvent event) {
    bool consumed = false;

    InfraredApp* infrared = context;

    if(event.type == SceneManagerEventTypeCustom) {
        const uint16_t custom_event_type = infrared_custom_event_get_type(event.event);
        const uint16_t custom_event_value = infrared_custom_event_get_value(event.event);

        if(custom_event_type == InfraredCustomEventTypeGpioTxPinChanged &&
           infrared_scene_gpio_settings_has_ext_pin()) {
            /* Saved at once (also for CLI / JS / plugins and after reboot). */
            const FuriHalInfraredTxPin tx_pin = custom_event_value ?
                                                    FuriHalInfraredTxPinExtPA7 :
                                                    FuriHalInfraredTxPinInternal;
            furi_hal_infrared_save_tx_output(tx_pin);
            infrared->app_state.tx_pin = tx_pin;
        } else if(custom_event_type == InfraredCustomEventTypeGpioTxPinChanged) {
            infrared_set_tx_pin(infrared, custom_event_value);
            variable_item_list_reset(infrared->var_item_list);
            infrared_scene_gpio_settings_init(infrared);
        } else if(custom_event_type == InfraredCustomEventTypeGpioOtgChanged) {
            infrared_set_vbus_5v_enabled(infrared, custom_event_value);
        }

        consumed = true;
    }

    return consumed;
}

void infrared_scene_gpio_settings_on_exit(void* context) {
    InfraredApp* infrared = context;
    variable_item_list_reset(infrared->var_item_list);
    infrared_save_settings(infrared);
}
