#include "../nrf24_app.h"
#include <furi_hal_nrf24.h>
#include <furi_hal_resources.h>

/* Module = which NRF24 the app talks to; CSN pin / CE pin = which two header GPIOs an
 * external module is wired to (MISO/MOSI/SCK are the board module's shared SPI bus).
 * Mirrors Sub-GHz's Radio Settings scene; the pins are only claimed while a scene that
 * opens the radio (nrf24_hw_init/furi_hal_nrf24_session_begin) runs with Module =
 * External, and free otherwise. Paradoxt-only: the item is hidden elsewhere. */

#define MODULE_COUNT 2
static const char* const module_text[MODULE_COUNT] = {"Internal", "External"};

enum {
    RadioSettingsEventTypeMask = 0xF00,
    RadioSettingsEventIndexMask = 0x0FF,
    RadioSettingsEventApplyModule = 0x100,
    RadioSettingsEventApplyCsn = 0x200,
    RadioSettingsEventApplyCe = 0x300,
};

static VariableItem* module_item;
static VariableItem* csn_item;
static VariableItem* ce_item;
static uint8_t csn_index;
static uint8_t ce_index;

static uint8_t radio_settings_pin_at(uint8_t index) {
    return (uint8_t)gpio_pins[index].pin->pin;
}

static uint8_t radio_settings_index_of(uint8_t pin) {
    for(size_t i = 0; i < gpio_pins_count; i++) {
        if(gpio_pins[i].pin->pin == pin) return (uint8_t)i;
    }
    return 0;
}

static const char* radio_settings_error_text(FuriHalNrf24ExtResult result) {
    switch(result) {
    case FuriHalNrf24ExtNotDetected:
        return "No NRF24 found\non the chosen pins";
    case FuriHalNrf24ExtPinInUse:
        return "A pin is used by\nanother feature";
    case FuriHalNrf24ExtBusy:
        return "The NRF24 bus\nis busy";
    case FuriHalNrf24ExtBadConfig:
        return "CSN and CE must\nbe different pins";
    default:
        return "Not available";
    }
}

static void radio_settings_show_error_text(Nrf24App* app, const char* text) {
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "External module", 64, 4, AlignCenter, AlignTop);
    dialog_message_set_text(message, text, 64, 32, AlignCenter, AlignCenter);
    dialog_message_set_buttons(message, NULL, "OK", NULL);
    dialog_message_show(app->dialogs, message);
    dialog_message_free(message);
}

static void radio_settings_show_error(Nrf24App* app, FuriHalNrf24ExtResult result) {
    radio_settings_show_error_text(app, radio_settings_error_text(result));
}

static void radio_settings_module_text(VariableItem* item, uint8_t index) {
    variable_item_set_current_value_text(item, module_text[index ? 1 : 0]);
}

/* Probe whichever module is currently selected with a throwaway session. */
static FuriHalNrf24ExtResult radio_settings_probe(void) {
    const FuriHalNrf24ExtResult result = furi_hal_nrf24_session_begin();
    if(result == FuriHalNrf24ExtOk) furi_hal_nrf24_session_end();
    return result;
}

/* Keep the requested route selected; report that the external module is unavailable. */
static void radio_settings_failed(Nrf24App* app, FuriHalNrf24ExtResult result) {
    if(module_item) {
        variable_item_set_current_value_index(module_item, 1);
        variable_item_set_current_value_text(module_item, "Ext: not found");
    }
    radio_settings_show_error(app, result);
}

static void radio_settings_apply_module(Nrf24App* app, uint8_t index) {
    FuriHalNrf24ExtConfig config;
    furi_hal_nrf24_ext_get_config(&config);

    if(index == 0) {
        config.use_external = false;
        furi_hal_nrf24_ext_set_config(&config);
        return;
    }

    /* session_begin() probes the configured route, so save the requested route first. */
    config.use_external = true;
    if(!furi_hal_nrf24_ext_set_config(&config)) {
        if(module_item) {
            variable_item_set_current_value_index(module_item, 0);
            radio_settings_module_text(module_item, 0);
        }
        radio_settings_show_error_text(app, "Could not save\nexternal selection");
        return;
    }

    const FuriHalNrf24ExtResult probe = radio_settings_probe();
    if(probe != FuriHalNrf24ExtOk) radio_settings_failed(app, probe);
}

static void radio_settings_set_module(VariableItem* item) {
    Nrf24App* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    radio_settings_module_text(item, index);
    /* Run SPI probes and modal dialogs from on_event, not inside list input dispatch. */
    view_dispatcher_send_custom_event(
        app->view_dispatcher, RadioSettingsEventApplyModule | index);
}

/* Step over the pad the other signal uses, in the direction the user was moving. */
static uint8_t radio_settings_pick(uint8_t index, uint8_t previous, uint8_t other) {
    const size_t count = gpio_pins_count;
    const bool forward = ((index + count - previous) % count) == 1;
    for(size_t guard = 0; guard < count && index == other; guard++) {
        index = forward ? (uint8_t)((index + 1) % count) : (uint8_t)((index + count - 1) % count);
    }
    return index;
}

/* Returns false when the pin was NOT persisted (a running session holds the old pins, or
 * the chosen pin is claimed by another external module, e.g. Sub-GHz's). The caller must
 * then put the displayed index/text back, or the screen would show a pin that was never
 * actually saved. */
static bool radio_settings_apply_pins(Nrf24App* app, bool csn_changed, uint8_t index) {
    FuriHalNrf24ExtConfig config;
    furi_hal_nrf24_ext_get_config(&config);
    const bool was_external = config.use_external;
    const uint8_t previous_index =
        radio_settings_index_of(csn_changed ? config.csn_pin : config.ce_pin);

    if(csn_changed) {
        config.csn_pin = radio_settings_pin_at(index);
    } else {
        config.ce_pin = radio_settings_pin_at(index);
    }
    if(!furi_hal_nrf24_ext_set_config(&config)) {
        if(csn_changed) {
            csn_index = previous_index;
            if(csn_item) {
                variable_item_set_current_value_index(csn_item, csn_index);
                variable_item_set_current_value_text(csn_item, gpio_pins[csn_index].name);
            }
        } else {
            ce_index = previous_index;
            if(ce_item) {
                variable_item_set_current_value_index(ce_item, ce_index);
                variable_item_set_current_value_text(ce_item, gpio_pins[ce_index].name);
            }
        }
        radio_settings_show_error_text(app, "Pin busy or used\nby another feature");
        return false;
    }

    if(csn_changed) {
        csn_index = index;
        if(csn_item) {
            variable_item_set_current_value_index(csn_item, csn_index);
            variable_item_set_current_value_text(csn_item, gpio_pins[csn_index].name);
        }
    } else {
        ce_index = index;
        if(ce_item) {
            variable_item_set_current_value_index(ce_item, ce_index);
            variable_item_set_current_value_text(ce_item, gpio_pins[ce_index].name);
        }
    }

    if(was_external) {
        const FuriHalNrf24ExtResult probe = radio_settings_probe();
        if(probe != FuriHalNrf24ExtOk) radio_settings_failed(app, probe);
    }
    return true;
}

static void radio_settings_set_csn(VariableItem* item) {
    Nrf24App* app = variable_item_get_context(item);
    csn_index = radio_settings_pick(variable_item_get_current_value_index(item), csn_index, ce_index);
    variable_item_set_current_value_index(item, csn_index);
    variable_item_set_current_value_text(item, gpio_pins[csn_index].name);
    view_dispatcher_send_custom_event(
        app->view_dispatcher, RadioSettingsEventApplyCsn | csn_index);
}

static void radio_settings_set_ce(VariableItem* item) {
    Nrf24App* app = variable_item_get_context(item);
    ce_index = radio_settings_pick(variable_item_get_current_value_index(item), ce_index, csn_index);
    variable_item_set_current_value_index(item, ce_index);
    variable_item_set_current_value_text(item, gpio_pins[ce_index].name);
    view_dispatcher_send_custom_event(
        app->view_dispatcher, RadioSettingsEventApplyCe | ce_index);
}

void nrf24_app_scene_radio_settings_on_enter(void* context) {
    Nrf24App* app = context;
    VariableItemList* vil = app->var_item_list;

    variable_item_list_reset(vil);
    variable_item_list_set_header(vil, "Radio Settings");

    FuriHalNrf24ExtConfig config;
    furi_hal_nrf24_ext_get_config(&config);

    const uint8_t module_index = config.use_external ? 1 : 0;
    module_item =
        variable_item_list_add(vil, "Module", MODULE_COUNT, radio_settings_set_module, app);
    variable_item_set_current_value_index(module_item, module_index);
    radio_settings_module_text(module_item, module_index);

    csn_index = radio_settings_index_of(config.csn_pin);
    ce_index = radio_settings_index_of(config.ce_pin);
    csn_item =
        variable_item_list_add(vil, "Ext CSN pin", gpio_pins_count, radio_settings_set_csn, app);
    variable_item_set_current_value_index(csn_item, csn_index);
    variable_item_set_current_value_text(csn_item, gpio_pins[csn_index].name);
    ce_item =
        variable_item_list_add(vil, "Ext CE pin", gpio_pins_count, radio_settings_set_ce, app);
    variable_item_set_current_value_index(ce_item, ce_index);
    variable_item_set_current_value_text(ce_item, gpio_pins[ce_index].name);

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewJamConfig);
}

bool nrf24_app_scene_radio_settings_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    const uint32_t event_type = event.event & RadioSettingsEventTypeMask;
    const uint8_t index = event.event & RadioSettingsEventIndexMask;

    switch(event_type) {
    case RadioSettingsEventApplyModule:
        radio_settings_apply_module(app, index);
        return true;
    case RadioSettingsEventApplyCsn:
        (void)radio_settings_apply_pins(app, true, index);
        return true;
    case RadioSettingsEventApplyCe:
        (void)radio_settings_apply_pins(app, false, index);
        return true;
    default:
        break;
    }

    return false;
}

void nrf24_app_scene_radio_settings_on_exit(void* context) {
    Nrf24App* app = context;
    module_item = NULL;
    csn_item = NULL;
    ce_item = NULL;
    variable_item_list_set_selected_item(app->var_item_list, 0);
    variable_item_list_reset(app->var_item_list);
}
