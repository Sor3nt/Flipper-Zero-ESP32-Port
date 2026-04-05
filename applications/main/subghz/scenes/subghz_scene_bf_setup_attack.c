#include "../subghz_i.h"
#include "../helpers/subbrute_custom_event.h"

#define TAG "SubGhzSceneBfSetupAttack"

static void subghz_scene_bf_setup_attack_callback(SubBruteCustomEvent event, void* context) {
    furi_assert(context);

    SubGhz* subghz = (SubGhz*)context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

static void
    subghz_scene_bf_setup_attack_device_state_changed(void* context, SubBruteWorkerState state) {
    furi_assert(context);

    SubGhz* subghz = (SubGhz*)context;

    if(state == SubBruteWorkerStateIDLE) {
        // Can't be IDLE on this step!
        view_dispatcher_send_custom_event(subghz->view_dispatcher, SubBruteCustomEventTypeError);
    }
}

void subghz_scene_bf_setup_attack_on_enter(void* context) {
    furi_assert(context);
    SubGhz* subghz = (SubGhz*)context;
    SubBruteAttackView* view = subghz->subbrute_attack_view;

    notification_message(subghz->notifications, &sequence_reset_vibro);

#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Enter Attack: %s", subbrute_protocol_name(subghz->subbrute_device->attack));
#endif

    subbrute_worker_set_callback(
        subghz->subbrute_worker, subghz_scene_bf_setup_attack_device_state_changed, context);
    if(subbrute_worker_is_running(subghz->subbrute_worker)) {
        subbrute_worker_stop(subghz->subbrute_worker);
        subghz->subbrute_device->current_step =
            subbrute_worker_get_step(subghz->subbrute_worker);
    }

    subbrute_attack_view_init_values(
        view,
        subghz->subbrute_device->attack,
        subghz->subbrute_device->max_value,
        subghz->subbrute_device->current_step,
        false,
        subbrute_worker_get_repeats(subghz->subbrute_worker));

    subbrute_attack_view_set_callback(view, subghz_scene_bf_setup_attack_callback, subghz);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdBfAttack);
}

void subghz_scene_bf_setup_attack_on_exit(void* context) {
    furi_assert(context);
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "subghz_scene_bf_setup_attack_on_exit");
#endif
    SubGhz* subghz = (SubGhz*)context;
    subbrute_worker_stop(subghz->subbrute_worker);
    notification_message(subghz->notifications, &sequence_blink_stop);
    notification_message(subghz->notifications, &sequence_reset_vibro);
}

bool subghz_scene_bf_setup_attack_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = (SubGhz*)context;
    SubBruteAttackView* view = subghz->subbrute_attack_view;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubBruteCustomEventTypeTransmitStarted) {
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneBfRunAttack);
        } else if(event.event == SubBruteCustomEventTypeSaveFile) {
            subbrute_attack_view_init_values(
                view,
                subghz->subbrute_device->attack,
                subghz->subbrute_device->max_value,
                subghz->subbrute_device->current_step,
                false,
                subghz->subbrute_device->extra_repeats);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneBfSaveName);
        } else if(event.event == SubBruteCustomEventTypeExtraSettings) {
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneBfSetupExtra);
        } else if(event.event == SubBruteCustomEventTypeBackPressed) {
            subbrute_attack_view_init_values(
                view,
                subghz->subbrute_device->attack,
                subghz->subbrute_device->max_value,
                subghz->subbrute_device->current_step,
                false,
                subghz->subbrute_device->extra_repeats);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneBfStart);
        } else if(event.event == SubBruteCustomEventTypeError) {
            notification_message(subghz->notifications, &sequence_error);
        } else if(event.event == SubBruteCustomEventTypeTransmitCustom) {
            // We can transmit only in not working states
            if(subbrute_worker_can_manual_transmit(subghz->subbrute_worker)) {
                // MANUAL Transmit!
                // Blink
                notification_message(subghz->notifications, &sequence_blink_green_100);
                subbrute_worker_transmit_current_key(
                    subghz->subbrute_worker, subghz->subbrute_device->current_step);
                // Stop
                notification_message(subghz->notifications, &sequence_blink_stop);
            }
        } else if(event.event == SubBruteCustomEventTypeChangeStepUp) {
            // +1
            uint64_t step = subbrute_device_add_step(subghz->subbrute_device, 1);
            subbrute_worker_set_step(subghz->subbrute_worker, step);
            subbrute_attack_view_set_current_step(view, step);
        } else if(event.event == SubBruteCustomEventTypeChangeStepUpMore) {
            // +50
            uint64_t step = subbrute_device_add_step(subghz->subbrute_device, 50);
            subbrute_worker_set_step(subghz->subbrute_worker, step);
            subbrute_attack_view_set_current_step(view, step);
        } else if(event.event == SubBruteCustomEventTypeChangeStepDown) {
            // -1
            uint64_t step = subbrute_device_add_step(subghz->subbrute_device, -1);
            subbrute_worker_set_step(subghz->subbrute_worker, step);
            subbrute_attack_view_set_current_step(view, step);
        } else if(event.event == SubBruteCustomEventTypeChangeStepDownMore) {
            // -50
            uint64_t step = subbrute_device_add_step(subghz->subbrute_device, -50);
            subbrute_worker_set_step(subghz->subbrute_worker, step);
            subbrute_attack_view_set_current_step(view, step);
        }

        consumed = true;
    } else if(event.type == SceneManagerEventTypeTick) {
        if(subbrute_worker_is_running(subghz->subbrute_worker)) {
            subghz->subbrute_device->current_step =
                subbrute_worker_get_step(subghz->subbrute_worker);
        }
        subbrute_attack_view_set_current_step(view, subghz->subbrute_device->current_step);
        consumed = true;
    }

    return consumed;
}
