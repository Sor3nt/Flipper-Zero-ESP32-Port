#include "../subghz_i.h"
#include "../helpers/subbrute_custom_event.h"

#define TAG "SubGhzSceneBfStart"

void subghz_scene_bf_start_callback(SubBruteCustomEvent event, void* context) {
    furi_assert(context);

    SubGhz* subghz = (SubGhz*)context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

void subghz_scene_bf_start_on_enter(void* context) {
    furi_assert(context);
#ifdef FURI_DEBUG
    FURI_LOG_I(TAG, "subghz_scene_bf_start_on_enter");
#endif
    SubGhz* subghz = (SubGhz*)context;
    SubBruteMainView* view = subghz->subbrute_main_view;

    subbrute_main_view_set_callback(view, subghz_scene_bf_start_callback, subghz);

    subghz->subbrute_device->attack = subghz->subbrute_settings->last_index;

    subbrute_main_view_set_index(
        view,
        subghz->subbrute_settings->last_index,
        subghz->subbrute_settings->repeat_values,
        false,
        subghz->subbrute_device->two_bytes,
        0);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdBfMain);
}

void subghz_scene_bf_start_on_exit(void* context) {
    furi_assert(context);
}

bool subghz_scene_bf_start_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = (SubGhz*)context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
#ifdef FURI_DEBUG
        FURI_LOG_D(
            TAG,
            "Event: %ld, SubBruteCustomEventTypeMenuSelected: %s, SubBruteCustomEventTypeLoadFile: %s",
            event.event,
            event.event == SubBruteCustomEventTypeMenuSelected ? "true" : "false",
            event.event == SubBruteCustomEventTypeLoadFile ? "true" : "false");
#endif
        if(event.event == SubBruteCustomEventTypeMenuSelected) {
            subghz->subbrute_settings->last_index =
                subbrute_main_view_get_index(subghz->subbrute_main_view);
            subbrute_settings_set_repeats(
                subghz->subbrute_settings,
                subbrute_main_view_get_repeats(subghz->subbrute_main_view));
            uint8_t total_repeats =
                subbrute_settings_get_current_repeats(subghz->subbrute_settings);

            if((subbrute_device_attack_set(
                    subghz->subbrute_device,
                    subghz->subbrute_settings->last_index,
                    total_repeats) != SubBruteFileResultOk) ||
               (!subbrute_worker_init_default_attack(
                   subghz->subbrute_worker,
                   subghz->subbrute_settings->last_index,
                   subghz->subbrute_device->current_step,
                   subghz->subbrute_device->protocol_info,
                   subghz->subbrute_device->extra_repeats))) {
                furi_crash("Invalid attack set!");
            }
            subbrute_settings_save(subghz->subbrute_settings);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneBfSetupAttack);

            consumed = true;
        } else if(event.event == SubBruteCustomEventTypeLoadFile) {
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneBfLoadFile);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        subghz->subbrute_settings->last_index =
            subbrute_main_view_get_index(subghz->subbrute_main_view);
        subbrute_settings_set_repeats(
            subghz->subbrute_settings,
            subbrute_main_view_get_repeats(subghz->subbrute_main_view));
        subbrute_settings_save(subghz->subbrute_settings);

        scene_manager_search_and_switch_to_previous_scene(
            subghz->scene_manager, SubGhzSceneStart);
        consumed = true;
    }

    return consumed;
}
