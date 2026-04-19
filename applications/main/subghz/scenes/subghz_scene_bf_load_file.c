#include "../subghz_i.h"
#include "../helpers/subbrute_custom_event.h"
#include "../helpers/subbrute_device.h"

#define TAG "SubGhzSceneBfLoadFile"

void subghz_scene_bf_load_file_on_enter(void* context) {
    furi_assert(context);
    SubGhz* subghz = (SubGhz*)context;

    // Input events and views are managed by file_browser
    FuriString* app_folder;
    FuriString* load_path;
    load_path = furi_string_alloc();
    app_folder = furi_string_alloc_set(SUBBRUTE_PATH);

    DialogsFileBrowserOptions browser_options;
    dialog_file_browser_set_basic_options(&browser_options, SUBBRUTE_FILE_EXT, &I_sub1_10px);

    SubBruteFileResult load_result;
#ifdef SUBBRUTE_FAST_TRACK
    bool res = true;
    furi_string_printf(load_path, "%s", "/ext/subghz/princeton.sub");
#else
    bool res =
        dialog_file_browser_show(subghz->dialogs, load_path, app_folder, &browser_options);
#endif
#ifdef FURI_DEBUG
    FURI_LOG_D(
        TAG,
        "load_path: %s, app_folder: %s",
        furi_string_get_cstr(load_path),
        furi_string_get_cstr(app_folder));
#endif
    if(res) {
        load_result =
            subbrute_device_load_from_file(subghz->subbrute_device, furi_string_get_cstr(load_path));
        if(load_result == SubBruteFileResultOk) {
            subghz->subbrute_settings->last_index = SubBruteAttackLoadFile;
            subbrute_settings_set_repeats(
                subghz->subbrute_settings,
                subbrute_main_view_get_repeats(subghz->subbrute_main_view));
            uint8_t extra_repeats =
                subbrute_settings_get_current_repeats(subghz->subbrute_settings);

            load_result = subbrute_device_attack_set(
                subghz->subbrute_device, subghz->subbrute_settings->last_index, extra_repeats);
            if(load_result == SubBruteFileResultOk) {
                if(!subbrute_worker_init_file_attack(
                       subghz->subbrute_worker,
                       subghz->subbrute_device->current_step,
                       subghz->subbrute_device->bit_index,
                       subghz->subbrute_device->key_from_file,
                       subghz->subbrute_device->file_protocol_info,
                       extra_repeats,
                       subghz->subbrute_device->two_bytes)) {
                    furi_crash("Invalid attack set!");
                }
                // Ready to run!
                FURI_LOG_I(TAG, "Ready to run");
                load_result = true;
            }
        }

        if(load_result == SubBruteFileResultOk) {
            subbrute_settings_save(subghz->subbrute_settings);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneBfLoadSelect);
        } else {
            FURI_LOG_E(TAG, "Returned error: %d", load_result);

            FuriString* dialog_msg;
            dialog_msg = furi_string_alloc();
            furi_string_cat_printf(
                dialog_msg, "Cannot parse\nfile: %s", subbrute_device_error_get_desc(load_result));
            dialog_message_show_storage_error(subghz->dialogs, furi_string_get_cstr(dialog_msg));
            furi_string_free(dialog_msg);
            scene_manager_search_and_switch_to_previous_scene(
                subghz->scene_manager, SubGhzSceneBfStart);
        }
    } else {
        scene_manager_search_and_switch_to_previous_scene(
            subghz->scene_manager, SubGhzSceneBfStart);
    }

    furi_string_free(app_folder);
    furi_string_free(load_path);
}

void subghz_scene_bf_load_file_on_exit(void* context) {
    UNUSED(context);
}

bool subghz_scene_bf_load_file_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);

    return false;
}
