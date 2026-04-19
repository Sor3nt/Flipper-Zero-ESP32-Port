#include "../subghz_i.h"

static void subghz_scene_jammer_callback(SubGhzCustomEvent event, void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

void subghz_scene_jammer_on_enter(void* context) {
    SubGhz* subghz = context;

    subghz_jammer_set_callback(subghz->subghz_jammer, subghz_scene_jammer_callback, subghz);
    subghz_jammer_start(subghz->subghz_jammer);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdJammer);
}

bool subghz_scene_jammer_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventViewReceiverBack) {
            scene_manager_search_and_switch_to_previous_scene(
                subghz->scene_manager, SubGhzSceneStart);
            return true;
        }
    }
    return false;
}

void subghz_scene_jammer_on_exit(void* context) {
    SubGhz* subghz = context;
    subghz_jammer_stop(subghz->subghz_jammer);
}
