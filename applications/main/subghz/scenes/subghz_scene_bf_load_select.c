#include "../subghz_i.h"
#include "../helpers/subbrute_custom_event.h"

#define TAG "SubGhzSceneBfLoadSelect"

void subghz_scene_bf_load_select_callback(SubBruteCustomEvent event, void* context) {
    furi_assert(context);

    SubGhz* subghz = (SubGhz*)context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

void subghz_scene_bf_load_select_on_enter(void* context) {
    furi_assert(context);
#ifdef FURI_DEBUG
    FURI_LOG_I(TAG, "subghz_scene_bf_load_select_on_enter");
#endif
    SubGhz* subghz = (SubGhz*)context;
    SubBruteMainView* view = subghz->subbrute_main_view;

    subbrute_main_view_set_callback(view, subghz_scene_bf_load_select_callback, subghz);
    subbrute_main_view_set_index(
        view,
        7,
        subghz->subbrute_settings->repeat_values,
        true,
        subghz->subbrute_device->two_bytes,
        subghz->subbrute_device->key_from_file);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdBfMain);
}

void subghz_scene_bf_load_select_on_exit(void* context) {
    UNUSED(context);
#ifdef FURI_DEBUG
    FURI_LOG_I(TAG, "subghz_scene_bf_load_select_on_exit");
#endif
}

bool subghz_scene_bf_load_select_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = (SubGhz*)context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubBruteCustomEventTypeIndexSelected) {
            subghz->subbrute_device->current_step = 0;
            subghz->subbrute_device->bit_index =
                subbrute_main_view_get_index(subghz->subbrute_main_view);
            subghz->subbrute_device->two_bytes =
                subbrute_main_view_get_two_bytes(subghz->subbrute_main_view);

            subghz->subbrute_settings->last_index = subghz->subbrute_device->attack;
            subbrute_settings_set_repeats(
                subghz->subbrute_settings,
                subbrute_main_view_get_repeats(subghz->subbrute_main_view));
            uint8_t total_repeats =
                subbrute_settings_get_current_repeats(subghz->subbrute_settings);

            subghz->subbrute_device->max_value = subbrute_protocol_calc_max_value(
                subghz->subbrute_device->attack,
                subghz->subbrute_device->bit_index,
                subghz->subbrute_device->two_bytes);

            if(!subbrute_worker_init_file_attack(
                   subghz->subbrute_worker,
                   subghz->subbrute_device->current_step,
                   subghz->subbrute_device->bit_index,
                   subghz->subbrute_device->key_from_file,
                   subghz->subbrute_device->file_protocol_info,
                   total_repeats,
                   subghz->subbrute_device->two_bytes)) {
                furi_crash("Invalid attack set!");
            }
            subbrute_settings_save(subghz->subbrute_settings);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneBfSetupAttack);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(!scene_manager_search_and_switch_to_previous_scene(
               subghz->scene_manager, SubGhzSceneBfStart)) {
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneBfStart);
        }
        consumed = true;
    }

    return consumed;
}
