#include "../subghz_i.h"
#include <lib/subghz/protocols/schrader_gg4.h>

#define TAG "SubGhzSceneTpmsEdit"

static void
    subghz_scene_tpms_edit_number_callback(void* context, int32_t number) {
    furi_assert(context);
    SubGhz* subghz = context;
    subghz->tpms_edit_number_value = number;
    view_dispatcher_send_custom_event(
        subghz->view_dispatcher, SubGhzCustomEventNumberInputDone);
}

static void subghz_scene_tpms_edit_byte_callback(void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, SubGhzCustomEventByteInputDone);
}

// Pull the current TPMS values from the selected history item into `generic`.
// Caller must have set generic->protocol_name beforehand if it cares about
// the re-serialized "Protocol" field.
static bool subghz_scene_tpms_edit_load_generic(SubGhz* subghz, TPMSBlockGeneric* generic) {
    FlipperFormat* fff =
        subghz_history_get_raw_data(subghz->history, subghz->idx_menu_chosen);
    if(!fff) {
        FURI_LOG_E(TAG, "No history fff");
        return false;
    }
    return tpms_block_generic_deserialize(generic, fff) == SubGhzProtocolStatusOk;
}

void subghz_scene_tpms_edit_on_enter(void* context) {
    SubGhz* subghz = context;
    TPMSBlockGeneric generic = {0};
    generic.protocol_name = subghz_history_get_protocol_name(
        subghz->history, subghz->idx_menu_chosen);
    if(!subghz_scene_tpms_edit_load_generic(subghz, &generic)) {
        // Bail out gracefully — go back without editing.
        scene_manager_previous_scene(subghz->scene_manager);
        return;
    }

    switch(subghz->tpms_edit_field) {
    case SubGhzViewTpmsFieldPressure: {
        // NumberInput operates on integers — we encode pressure as decibar
        // (0.0..44.0 bar → 0..440), which still beats the underlying raw
        // resolution (~0.17 bar/step).
        int32_t init = (int32_t)(generic.pressure * 10.0f + 0.5f);
        number_input_set_header_text(subghz->number_input, "Pressure x10 (bar)");
        number_input_set_result_callback(
            subghz->number_input,
            subghz_scene_tpms_edit_number_callback,
            subghz,
            init,
            0,
            440);
        view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdNumberInput);
        break;
    }
    case SubGhzViewTpmsFieldTemperature: {
        int32_t init = (int32_t)(generic.temperature + (generic.temperature >= 0 ? 0.5f : -0.5f));
        number_input_set_header_text(subghz->number_input, "Temperature (C)");
        number_input_set_result_callback(
            subghz->number_input,
            subghz_scene_tpms_edit_number_callback,
            subghz,
            init,
            -50,
            205);
        view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdNumberInput);
        break;
    }
    case SubGhzViewTpmsFieldId: {
        // Pre-fill the byte buffer with the current ID in big-endian.
        uint32_t id = generic.id;
        subghz->tpms_edit_id_bytes[0] = (id >> 24) & 0xFF;
        subghz->tpms_edit_id_bytes[1] = (id >> 16) & 0xFF;
        subghz->tpms_edit_id_bytes[2] = (id >> 8) & 0xFF;
        subghz->tpms_edit_id_bytes[3] = (id >> 0) & 0xFF;
        byte_input_set_header_text(subghz->byte_input, "Edit ID (hex)");
        byte_input_set_result_callback(
            subghz->byte_input,
            subghz_scene_tpms_edit_byte_callback,
            NULL,
            subghz,
            subghz->tpms_edit_id_bytes,
            sizeof(subghz->tpms_edit_id_bytes));
        view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdByteInput);
        break;
    }
    default:
        FURI_LOG_E(TAG, "Unsupported field for editor: %d", (int)subghz->tpms_edit_field);
        scene_manager_previous_scene(subghz->scene_manager);
        break;
    }
}

bool subghz_scene_tpms_edit_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(event.event != SubGhzCustomEventNumberInputDone &&
       event.event != SubGhzCustomEventByteInputDone) {
        return false;
    }

    TPMSBlockGeneric generic = {0};
    const char* protocol_name =
        subghz_history_get_protocol_name(subghz->history, subghz->idx_menu_chosen);
    generic.protocol_name = protocol_name;
    if(!subghz_scene_tpms_edit_load_generic(subghz, &generic)) {
        scene_manager_previous_scene(subghz->scene_manager);
        return true;
    }
    // protocol_name is overwritten by deserialize-into-bare-struct only via the
    // FFF "Protocol" key; the deserialize function does not actually touch this
    // field, so reset it explicitly to match what we want serialized back.
    generic.protocol_name = protocol_name;

    switch(subghz->tpms_edit_field) {
    case SubGhzViewTpmsFieldPressure:
        // decibar → bar
        generic.pressure = (float)subghz->tpms_edit_number_value / 10.0f;
        tpms_protocol_schrader_gg4_pack(&generic);
        break;
    case SubGhzViewTpmsFieldTemperature:
        generic.temperature = (float)subghz->tpms_edit_number_value;
        tpms_protocol_schrader_gg4_pack(&generic);
        break;
    case SubGhzViewTpmsFieldId: {
        uint32_t id = ((uint32_t)subghz->tpms_edit_id_bytes[0] << 24) |
                      ((uint32_t)subghz->tpms_edit_id_bytes[1] << 16) |
                      ((uint32_t)subghz->tpms_edit_id_bytes[2] << 8) |
                      ((uint32_t)subghz->tpms_edit_id_bytes[3]);
        generic.id = id;
        tpms_protocol_schrader_gg4_pack(&generic);
        break;
    }
    default:
        break;
    }

    if(!subghz_history_replace_tpms_payload(
           subghz->history, subghz->idx_menu_chosen, &generic)) {
        FURI_LOG_E(TAG, "history replace failed");
    }

    scene_manager_previous_scene(subghz->scene_manager);
    return true;
}

void subghz_scene_tpms_edit_on_exit(void* context) {
    SubGhz* subghz = context;
    // Detach callbacks so the modules can be re-used by other scenes safely.
    number_input_set_result_callback(subghz->number_input, NULL, NULL, 0, 0, 0);
    number_input_set_header_text(subghz->number_input, "");
    byte_input_set_result_callback(subghz->byte_input, NULL, NULL, NULL, NULL, 0);
    byte_input_set_header_text(subghz->byte_input, "");
}
