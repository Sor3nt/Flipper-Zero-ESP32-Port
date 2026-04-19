#include "../subghz_i.h"
#include "../helpers/subbrute_custom_event.h"

#define TAG "SubGhzSceneBfRunAttack"

static void subghz_scene_bf_run_attack_callback(SubBruteCustomEvent event, void* context) {
    furi_assert(context);

    SubGhz* subghz = (SubGhz*)context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

static void
    subghz_scene_bf_run_attack_device_state_changed(void* context, SubBruteWorkerState state) {
    furi_assert(context);

    SubGhz* subghz = (SubGhz*)context;

    if(state == SubBruteWorkerStateIDLE) {
        // Can't be IDLE on this step!
        view_dispatcher_send_custom_event(subghz->view_dispatcher, SubBruteCustomEventTypeError);
    } else if(state == SubBruteWorkerStateFinished) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubBruteCustomEventTypeTransmitFinished);
    }
}

void subghz_scene_bf_run_attack_on_exit(void* context) {
    furi_assert(context);
    SubGhz* subghz = (SubGhz*)context;
    notification_message(subghz->notifications, &sequence_blink_stop);
    subbrute_worker_stop(subghz->subbrute_worker);
}

void subghz_scene_bf_run_attack_on_enter(void* context) {
    furi_assert(context);
    SubGhz* subghz = (SubGhz*)context;
    SubBruteAttackView* view = subghz->subbrute_attack_view;

    subbrute_attack_view_set_callback(view, subghz_scene_bf_run_attack_callback, subghz);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdBfAttack);

    subbrute_worker_set_callback(
        subghz->subbrute_worker, subghz_scene_bf_run_attack_device_state_changed, subghz);

    if(!subbrute_worker_is_running(subghz->subbrute_worker)) {
        subbrute_worker_set_step(
            subghz->subbrute_worker, subghz->subbrute_device->current_step);
        if(!subbrute_worker_start(subghz->subbrute_worker)) {
            view_dispatcher_send_custom_event(
                subghz->view_dispatcher, SubBruteCustomEventTypeError);
        } else {
            notification_message(subghz->notifications, &sequence_single_vibro);
            notification_message(subghz->notifications, &sequence_blink_start_yellow);
        }
    }
}

bool subghz_scene_bf_run_attack_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = (SubGhz*)context;
    SubBruteAttackView* view = subghz->subbrute_attack_view;

    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        uint64_t step = subbrute_worker_get_step(subghz->subbrute_worker);
        subghz->subbrute_device->current_step = step;
        subbrute_attack_view_set_current_step(view, step);

        if(event.event == SubBruteCustomEventTypeTransmitFinished) {
            notification_message(subghz->notifications, &sequence_display_backlight_on);
            notification_message(subghz->notifications, &sequence_double_vibro);

            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneBfSetupAttack);
        } else if(
            event.event == SubBruteCustomEventTypeTransmitNotStarted ||
            event.event == SubBruteCustomEventTypeBackPressed) {
            if(subbrute_worker_is_running(subghz->subbrute_worker)) {
                // Notify
                notification_message(subghz->notifications, &sequence_single_vibro);
            }
            // Stop transmit
            scene_manager_search_and_switch_to_previous_scene(
                subghz->scene_manager, SubGhzSceneBfSetupAttack);
        } else if(event.event == SubBruteCustomEventTypeError) {
            notification_message(subghz->notifications, &sequence_error);

            // Stop transmit
            scene_manager_search_and_switch_to_previous_scene(
                subghz->scene_manager, SubGhzSceneBfSetupAttack);
        } else if(event.event == SubBruteCustomEventTypeUpdateView) {
            //subbrute_attack_view_set_current_step(view, subghz->subbrute_device->current_step);
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeTick) {
        uint64_t step = subbrute_worker_get_step(subghz->subbrute_worker);
        subghz->subbrute_device->current_step = step;
        subbrute_attack_view_set_current_step(view, step);

        consumed = true;
    }

    return consumed;
}
