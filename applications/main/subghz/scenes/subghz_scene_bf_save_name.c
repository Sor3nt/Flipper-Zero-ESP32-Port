#include "../subghz_i.h"
#include "../helpers/subbrute_custom_event.h"
#include "../helpers/subbrute_device.h"
#include <toolbox/name_generator.h>

#define TAG "SubGhzSceneBfSaveName"

static void subghz_scene_bf_save_name_text_input_callback(void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(
        subghz->view_dispatcher, SubBruteCustomEventTypeTextEditDone);
}

void subghz_scene_bf_save_name_on_enter(void* context) {
    SubGhz* subghz = (SubGhz*)context;

    // Setup view
    TextInput* text_input = subghz->text_input;
    if(subghz->subbrute_device->attack == SubBruteAttackLoadFile) {
        name_generator_make_auto(
            subghz->file_name_tmp,
            sizeof(subghz->file_name_tmp),
            subbrute_protocol_file(subghz->subbrute_device->file_protocol_info->file));
    } else {
        name_generator_make_auto(
            subghz->file_name_tmp,
            sizeof(subghz->file_name_tmp),
            subbrute_protocol_file(subghz->subbrute_device->protocol_info->file));
    }
    text_input_set_header_text(text_input, "Name of file");
    text_input_set_result_callback(
        text_input,
        subghz_scene_bf_save_name_text_input_callback,
        subghz,
        subghz->file_name_tmp,
        SUBGHZ_MAX_LEN_NAME,
        true);

    furi_string_reset(subghz->file_path);
    furi_string_set_str(subghz->file_path, SUBBRUTE_PATH);

    ValidatorIsFile* validator_is_file = validator_is_file_alloc_init(
        furi_string_get_cstr(subghz->file_path), SUBBRUTE_FILE_EXT, "");
    text_input_set_validator(text_input, validator_is_file_callback, validator_is_file);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdTextInput);
}

bool subghz_scene_bf_save_name_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = (SubGhz*)context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeBack) {
        scene_manager_previous_scene(subghz->scene_manager);
        return true;
    } else if(
        event.type == SceneManagerEventTypeCustom &&
        event.event == SubBruteCustomEventTypeTextEditDone) {
#ifdef FURI_DEBUG
        FURI_LOG_D(TAG, "Saving: %s", subghz->file_name_tmp);
#endif
        bool success = false;
        if(strcmp(subghz->file_name_tmp, "") != 0) {
            furi_string_reset(subghz->file_path);
            furi_string_cat_printf(
                subghz->file_path,
                "%s/%s%s",
                SUBBRUTE_PATH,
                subghz->file_name_tmp,
                SUBBRUTE_FILE_EXT);

            if(subbrute_device_save_file(
                   subghz->subbrute_device, furi_string_get_cstr(subghz->file_path))) {
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneBfSaveSuccess);
                success = true;
                consumed = true;
            }
        }

        if(!success) {
            dialog_message_show_storage_error(subghz->dialogs, "Error during saving!");
            consumed = scene_manager_search_and_switch_to_previous_scene(
                subghz->scene_manager, SubGhzSceneBfSetupAttack);
        }
    }

    return consumed;
}

void subghz_scene_bf_save_name_on_exit(void* context) {
    SubGhz* subghz = (SubGhz*)context;

    // Clear view
    void* validator_context = text_input_get_validator_callback_context(subghz->text_input);
    text_input_set_validator(subghz->text_input, NULL, NULL);
    validator_is_file_free(validator_context);

    text_input_reset(subghz->text_input);

    furi_string_reset(subghz->file_path);
}
