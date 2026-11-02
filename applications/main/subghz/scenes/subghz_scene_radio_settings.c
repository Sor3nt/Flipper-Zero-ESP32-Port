#include "../subghz_i.h"
#include "../helpers/subghz_custom_event.h"
#include <lib/toolbox/value_index.h>
#include <applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.h>

#define RADIO_DEVICE_COUNT 2
const char* const radio_device_text[RADIO_DEVICE_COUNT] = {
    "Internal",
    "External",
};

const uint32_t radio_device_value[RADIO_DEVICE_COUNT] = {
    SubGhzRadioDeviceTypeInternal,
    SubGhzRadioDeviceTypeExternalCC1101,
};

#define ON_OFF_COUNT 2
const char* const on_off_text[ON_OFF_COUNT] = {
    "OFF",
    "ON",
};

#define DEBUG_P_COUNT 2
const char* const debug_pin_text[DEBUG_P_COUNT] = {
    "OFF",
    "17(1W)",
};

#define DEBUG_COUNTER_COUNT 17
const char* const debug_counter_text[DEBUG_COUNTER_COUNT] = {
    "+1",
    "+2",
    "+3",
    "+4",
    "+5",
    "+10",
    "+50",
    "OVFL",
    "OFEX",
    "No",
    "-1",
    "-2",
    "-3",
    "-4",
    "-5",
    "-10",
    "-50",
};
const int32_t debug_counter_val[DEBUG_COUNTER_COUNT] = {
    1,
    2,
    3,
    4,
    5,
    10,
    50,
    65535,
    -2147483647,
    0,
    -1,
    -2,
    -3,
    -4,
    -5,
    -10,
    -50,
};

//TX Power
#define TX_POWER_COUNT SUBGHZ_TXRX_TX_POWER_COUNT
const char* const tx_power_text[TX_POWER_COUNT] = {
    "Preset",
    "10dBm +",
    "7dBm",
    "5dBm",
    "0dBm",
    "-10dBm",
    "-15dBm",
    "-20dBm",
    "-30dBm",
};

// Frequency offset: -500kHz to +500kHz in 10kHz steps = 101 values, center at index 50
#define FREQ_OFFSET_COUNT 101
#define FREQ_OFFSET_CENTER 50
#define FREQ_OFFSET_STEP  10000 // 10 kHz

static void subghz_scene_radio_settings_set_freq_offset(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    int32_t offset_hz = ((int32_t)index - FREQ_OFFSET_CENTER) * FREQ_OFFSET_STEP;

    static char offset_str[16];
    if(offset_hz == 0) {
        snprintf(offset_str, sizeof(offset_str), "0 kHz");
    } else {
        snprintf(
            offset_str,
            sizeof(offset_str),
            "%+ld kHz",
            (long)(offset_hz / 1000));
    }
    variable_item_set_current_value_text(item, offset_str);

    subghz->last_settings->frequency_offset = offset_hz;
    subghz_txrx_set_frequency_offset(subghz->txrx, offset_hz);
    subghz_last_settings_save(subghz->last_settings);
}

static void subghz_scene_radio_settings_set_tx_power(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    //Update the Menu Item on screen
    variable_item_set_current_value_text(item, tx_power_text[index]);

    //Set TX power and remember setting
    subghz->last_settings->tx_power = subghz->tx_power = index;

    //Save the settings now, this is the convention here!
    subghz_last_settings_save(subghz->last_settings);
}

static void subghz_scene_receiver_config_set_debug_pin(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, debug_pin_text[index]);

    subghz_txrx_set_debug_pin_state(subghz->txrx, index == 1);
}

static void subghz_scene_reciever_config_set_ext_amp_leds_control(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, on_off_text[index]);
    subghz->last_settings->leds_and_amp = index == 1;
    // Set globally in furi hal
    furi_hal_subghz_set_ext_leds_and_amp(subghz->last_settings->leds_and_amp);
    subghz_last_settings_save(subghz->last_settings);
    // reinit external device
    const SubGhzRadioDeviceType current = subghz_txrx_radio_device_get(subghz->txrx);
    if(current != SubGhzRadioDeviceTypeInternal) {
        subghz_txrx_radio_device_set(subghz->txrx, SubGhzRadioDeviceTypeInternal);
        subghz_txrx_radio_device_set(subghz->txrx, current);
    }
}

static void subghz_scene_receiver_config_set_debug_counter(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, debug_counter_text[index]);
    furi_hal_subghz_set_rolling_counter_mult(debug_counter_val[index]);
}

static void subghz_scene_receiver_config_set_timestamp_file_names(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, on_off_text[index]);

    subghz->last_settings->protocol_file_names = (index == 1);
    subghz_last_settings_save(subghz->last_settings);
}

/* ---- External CC1101 module (boards with furi_hal_subghz_ext_supported()) ----
 * Module = which radio Sub-GHz uses; Ext CS / Ext GDO0 = which two header GPIOs the
 * module is wired to (MISO/MOSI/SCK are the board radio's shared SPI bus). The pins are
 * only stored here - they are claimed while the module is the active radio, i.e. for
 * the time the Sub-GHz app runs with Module = External, and free otherwise. */

static VariableItem* ext_module_item;
static VariableItem* ext_cs_item;
static VariableItem* ext_g0_item;
static uint8_t ext_cs_index;
static uint8_t ext_g0_index;
/* The change callbacks only update the on-screen value and stash the request
 * here; the blocking probe/attach and any dialog run later from on_event. */
static uint8_t ext_pending_module_index;
static uint8_t ext_cs_prev;
static uint8_t ext_g0_prev;

static uint8_t subghz_scene_radio_settings_ext_pin_at(uint8_t index) {
    return (uint8_t)gpio_pins[index].pin->pin;
}

static uint8_t subghz_scene_radio_settings_ext_index_of(uint8_t pin) {
    for(size_t i = 0; i < gpio_pins_count; i++) {
        if(gpio_pins[i].pin->pin == pin) return (uint8_t)i;
    }
    return 0;
}

static const char* subghz_scene_radio_settings_ext_error_text(FuriHalSubGhzExtResult result) {
    switch(result) {
    case FuriHalSubGhzExtNotDetected:
        return "No CC1101 found\non the chosen pins";
    case FuriHalSubGhzExtPinInUse:
        return "A pin is used by\nanother feature";
    case FuriHalSubGhzExtBusy:
        return "The radio is busy";
    case FuriHalSubGhzExtBadConfig:
        return "CS and GDO0 must\nbe different pins";
    default:
        return "Not available";
    }
}

static void subghz_scene_radio_settings_ext_show_error_text(SubGhz* subghz, const char* text) {
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "External module", 64, 4, AlignCenter, AlignTop);
    dialog_message_set_text(message, text, 64, 32, AlignCenter, AlignCenter);
    dialog_message_set_buttons(message, NULL, "OK", NULL);
    dialog_message_show(subghz->dialogs, message);
    dialog_message_free(message);
}

static void subghz_scene_radio_settings_ext_show_error(SubGhz* subghz, FuriHalSubGhzExtResult result) {
    subghz_scene_radio_settings_ext_show_error_text(
        subghz, subghz_scene_radio_settings_ext_error_text(result));
}

static void subghz_scene_radio_settings_ext_module_text(VariableItem* item, uint8_t index) {
    variable_item_set_current_value_text(item, radio_device_text[index ? 1 : 0]);
}

/* Fall back to the board radio, forget the External choice and tell the user why. */
static void subghz_scene_radio_settings_ext_failed(SubGhz* subghz, FuriHalSubGhzExtResult result) {
    FuriHalSubGhzExtConfig config;
    furi_hal_subghz_ext_get_config(&config);
    subghz_txrx_radio_device_set(subghz->txrx, SubGhzRadioDeviceTypeInternal);
    config.use_external = false;
    furi_hal_subghz_ext_set_config(&config);
    if(ext_module_item) {
        variable_item_set_current_value_index(ext_module_item, 0);
        subghz_scene_radio_settings_ext_module_text(ext_module_item, 0);
    }
    subghz_scene_radio_settings_ext_show_error(subghz, result);
}

/* Heavy path: blocking probe/attach + possible modal dialog. Must run from
 * on_event, never from the VariableItem change callback. */
static void subghz_scene_radio_settings_apply_module(SubGhz* subghz, uint8_t index) {
    FuriHalSubGhzExtConfig config;
    furi_hal_subghz_ext_get_config(&config);

    if(index == 0) {
        /* Releases the module and its pins; the board radio is used from here on. */
        subghz_txrx_radio_device_set(subghz->txrx, SubGhzRadioDeviceTypeInternal);
        config.use_external = false;
        furi_hal_subghz_ext_set_config(&config);
        return;
    }

    /* Probe first so the user gets the real reason when it does not work. */
    const FuriHalSubGhzExtResult probe = furi_hal_subghz_ext_probe();
    if(probe != FuriHalSubGhzExtOk) {
        subghz_scene_radio_settings_ext_failed(subghz, probe);
        return;
    }
    config.use_external = true;
    furi_hal_subghz_ext_set_config(&config);
    if(subghz_txrx_radio_device_set(subghz->txrx, SubGhzRadioDeviceTypeExternalCC1101) !=
       SubGhzRadioDeviceTypeExternalCC1101) {
        subghz_scene_radio_settings_ext_failed(subghz, FuriHalSubGhzExtBusy);
    }
}

static void subghz_scene_radio_settings_set_module(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    /* Only the cheap on-screen update happens here; the probe/attach runs from
     * on_event so the list's input dispatch isn't blocked and no modal dialog is
     * opened from inside it. */
    subghz_scene_radio_settings_ext_module_text(item, index);
    ext_pending_module_index = index;
    view_dispatcher_send_custom_event(
        subghz->view_dispatcher, SubGhzCustomEventSceneSettingExtModule);
}

/* Step over the pad the other signal uses, in the direction the user was moving. */
static uint8_t subghz_scene_radio_settings_ext_pick(uint8_t index, uint8_t previous, uint8_t other) {
    const size_t count = gpio_pins_count;
    const bool forward = ((index + count - previous) % count) == 1;
    for(size_t guard = 0; guard < count && index == other; guard++) {
        index = forward ? (uint8_t)((index + 1) % count) : (uint8_t)((index + count - 1) % count);
    }
    return index;
}

/* Returns false when the pin was NOT persisted (the module is attached and held, or the
 * chosen pin is claimed by another external module, e.g. NRF24's). The caller must then put
 * the displayed index/text back, or the screen would show a pin that was never saved. */
static bool subghz_scene_radio_settings_ext_apply_pins(SubGhz* subghz, bool cs_changed) {
    FuriHalSubGhzExtConfig config;
    furi_hal_subghz_ext_get_config(&config);
    const bool was_external =
        subghz_txrx_radio_device_get(subghz->txrx) == SubGhzRadioDeviceTypeExternalCC1101;

    /* The pins can only change while the module is released. */
    if(was_external) subghz_txrx_radio_device_set(subghz->txrx, SubGhzRadioDeviceTypeInternal);
    if(cs_changed) {
        config.cs_pin = subghz_scene_radio_settings_ext_pin_at(ext_cs_index);
    } else {
        config.g0_pin = subghz_scene_radio_settings_ext_pin_at(ext_g0_index);
    }
    if(!furi_hal_subghz_ext_set_config(&config)) {
        if(was_external) {
            /* Put the module back exactly as it was before this pick. */
            subghz_txrx_radio_device_set(subghz->txrx, SubGhzRadioDeviceTypeExternalCC1101);
        }
        subghz_scene_radio_settings_ext_show_error_text(subghz, "Pin busy or used\nby another feature");
        return false;
    }

    if(was_external &&
       subghz_txrx_radio_device_set(subghz->txrx, SubGhzRadioDeviceTypeExternalCC1101) !=
           SubGhzRadioDeviceTypeExternalCC1101) {
        subghz_scene_radio_settings_ext_failed(subghz, furi_hal_subghz_ext_probe());
    }
    return true;
}

static void subghz_scene_radio_settings_set_ext_cs(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    ext_cs_prev = ext_cs_index;
    ext_cs_index = subghz_scene_radio_settings_ext_pick(
        variable_item_get_current_value_index(item), ext_cs_index, ext_g0_index);
    variable_item_set_current_value_index(item, ext_cs_index);
    variable_item_set_current_value_text(item, gpio_pins[ext_cs_index].name);
    /* Persist + re-attach (and any error dialog) deferred to on_event. */
    view_dispatcher_send_custom_event(
        subghz->view_dispatcher, SubGhzCustomEventSceneSettingExtCs);
}

static void subghz_scene_radio_settings_set_ext_g0(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    ext_g0_prev = ext_g0_index;
    ext_g0_index = subghz_scene_radio_settings_ext_pick(
        variable_item_get_current_value_index(item), ext_g0_index, ext_cs_index);
    variable_item_set_current_value_index(item, ext_g0_index);
    variable_item_set_current_value_text(item, gpio_pins[ext_g0_index].name);
    /* Persist + re-attach (and any error dialog) deferred to on_event. */
    view_dispatcher_send_custom_event(
        subghz->view_dispatcher, SubGhzCustomEventSceneSettingExtG0);
}

static void subghz_scene_radio_settings_add_ext_items(SubGhz* subghz) {
    FuriHalSubGhzExtConfig config;
    furi_hal_subghz_ext_get_config(&config);

    const uint8_t module_index =
        (subghz_txrx_radio_device_get(subghz->txrx) == SubGhzRadioDeviceTypeExternalCC1101 ||
         config.use_external) ?
            1 :
            0;
    ext_module_item = variable_item_list_add(
        subghz->variable_item_list,
        "Module",
        RADIO_DEVICE_COUNT,
        subghz_scene_radio_settings_set_module,
        subghz);
    variable_item_set_current_value_index(ext_module_item, module_index);
    if(module_index == 1 &&
       subghz_txrx_radio_device_get(subghz->txrx) != SubGhzRadioDeviceTypeExternalCC1101) {
        /* Chosen earlier but not answering now (unplugged, pin taken). */
        variable_item_set_current_value_text(ext_module_item, "Ext: not found");
    } else {
        subghz_scene_radio_settings_ext_module_text(ext_module_item, module_index);
    }

    ext_cs_index = subghz_scene_radio_settings_ext_index_of(config.cs_pin);
    ext_g0_index = subghz_scene_radio_settings_ext_index_of(config.g0_pin);
    ext_cs_item = variable_item_list_add(
        subghz->variable_item_list,
        "Ext CS pin",
        gpio_pins_count,
        subghz_scene_radio_settings_set_ext_cs,
        subghz);
    variable_item_set_current_value_index(ext_cs_item, ext_cs_index);
    variable_item_set_current_value_text(ext_cs_item, gpio_pins[ext_cs_index].name);
    ext_g0_item = variable_item_list_add(
        subghz->variable_item_list,
        "Ext GDO0 pin",
        gpio_pins_count,
        subghz_scene_radio_settings_set_ext_g0,
        subghz);
    variable_item_set_current_value_index(ext_g0_item, ext_g0_index);
    variable_item_set_current_value_text(ext_g0_item, gpio_pins[ext_g0_index].name);
}

void subghz_scene_radio_settings_on_enter(void* context) {
    SubGhz* subghz = context;

    VariableItemList* variable_item_list = subghz->variable_item_list;
    int32_t value_index;
    VariableItem* item;

    if(furi_hal_subghz_ext_supported()) {
        subghz_scene_radio_settings_add_ext_items(subghz);
    }

    //Add TX Power
    item = variable_item_list_add(
        subghz->variable_item_list,
        "TX Power",
        TX_POWER_COUNT,
        subghz_scene_radio_settings_set_tx_power,
        subghz);

    value_index = subghz->tx_power < TX_POWER_COUNT ? subghz->tx_power : 0;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, tx_power_text[value_index]);

    // Frequency Offset (crystal calibration)
    item = variable_item_list_add(
        variable_item_list,
        "Freq Offset",
        FREQ_OFFSET_COUNT,
        subghz_scene_radio_settings_set_freq_offset,
        subghz);
    {
        int32_t offset_hz = subghz->last_settings->frequency_offset;
        uint8_t offset_idx =
            (uint8_t)CLAMP((offset_hz / FREQ_OFFSET_STEP) + FREQ_OFFSET_CENTER, FREQ_OFFSET_COUNT - 1, 0);
        variable_item_set_current_value_index(item, offset_idx);
        static char offset_str[16];
        if(offset_hz == 0) {
            snprintf(offset_str, sizeof(offset_str), "0 kHz");
        } else {
            snprintf(offset_str, sizeof(offset_str), "%+ld kHz", (long)(offset_hz / 1000));
        }
        variable_item_set_current_value_text(item, offset_str);
    }

    item = variable_item_list_add(
        variable_item_list,
        "Protocol Names",
        ON_OFF_COUNT,
        subghz_scene_receiver_config_set_timestamp_file_names,
        subghz);
    value_index = subghz->last_settings->protocol_file_names;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, on_off_text[value_index]);

    item = variable_item_list_add(
        variable_item_list,
        "Counter Incr.",
        furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug) ? DEBUG_COUNTER_COUNT : 3,
        subghz_scene_receiver_config_set_debug_counter,
        subghz);
    value_index = value_index_int32(
        furi_hal_subghz_get_rolling_counter_mult(),
        debug_counter_val,
        furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug) ? DEBUG_COUNTER_COUNT : 3);
    furi_hal_subghz_set_rolling_counter_mult(debug_counter_val[value_index]);

    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, debug_counter_text[value_index]);

    if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug)) {
        item = variable_item_list_add(
            variable_item_list,
            "Ext Amp & LEDs",
            ON_OFF_COUNT,
            subghz_scene_reciever_config_set_ext_amp_leds_control,
            subghz);
        value_index = subghz->last_settings->leds_and_amp ? 1 : 0;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, on_off_text[value_index]);

        item = variable_item_list_add(
            variable_item_list,
            "Debug Pin",
            DEBUG_P_COUNT,
            subghz_scene_receiver_config_set_debug_pin,
            subghz);
        value_index = subghz_txrx_get_debug_pin_state(subghz->txrx);
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, debug_pin_text[value_index]);
    }

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdVariableItemList);
}

bool subghz_scene_radio_settings_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case SubGhzCustomEventSceneSettingExtModule:
            subghz_scene_radio_settings_apply_module(subghz, ext_pending_module_index);
            return true;
        case SubGhzCustomEventSceneSettingExtCs:
            if(!subghz_scene_radio_settings_ext_apply_pins(subghz, true)) {
                /* Rejected: put the shown pin back to what was actually saved. */
                ext_cs_index = ext_cs_prev;
                if(ext_cs_item) {
                    variable_item_set_current_value_index(ext_cs_item, ext_cs_index);
                    variable_item_set_current_value_text(
                        ext_cs_item, gpio_pins[ext_cs_index].name);
                }
            }
            return true;
        case SubGhzCustomEventSceneSettingExtG0:
            if(!subghz_scene_radio_settings_ext_apply_pins(subghz, false)) {
                ext_g0_index = ext_g0_prev;
                if(ext_g0_item) {
                    variable_item_set_current_value_index(ext_g0_item, ext_g0_index);
                    variable_item_set_current_value_text(
                        ext_g0_item, gpio_pins[ext_g0_index].name);
                }
            }
            return true;
        default:
            break;
        }
    }

    return false;
}

void subghz_scene_radio_settings_on_exit(void* context) {
    SubGhz* subghz = context;
    ext_module_item = NULL;
    ext_cs_item = NULL;
    ext_g0_item = NULL;
    variable_item_list_set_selected_item(subghz->variable_item_list, 0);
    variable_item_list_reset(subghz->variable_item_list);
}
