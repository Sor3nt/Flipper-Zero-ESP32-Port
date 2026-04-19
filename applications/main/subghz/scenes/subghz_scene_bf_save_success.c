#include "../subghz_i.h"
#include "../helpers/subbrute_custom_event.h"

static void subghz_scene_bf_save_success_popup_callback(void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(
        subghz->view_dispatcher, SubBruteCustomEventTypePopupClosed);
}

void subghz_scene_bf_save_success_on_enter(void* context) {
    furi_assert(context);
    SubGhz* subghz = context;

    // Setup view
    Popup* popup = subghz->popup;
    popup_set_icon(popup, 32, 5, &I_DolphinSaved_92x58);
    popup_set_header(popup, "Saved!", 13, 22, AlignLeft, AlignBottom);
    popup_set_timeout(popup, 1500);
    popup_set_context(popup, subghz);
    popup_set_callback(popup, subghz_scene_bf_save_success_popup_callback);
    popup_enable_timeout(popup);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdPopup);
}

bool subghz_scene_bf_save_success_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);

    SubGhz* subghz = (SubGhz*)context;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubBruteCustomEventTypePopupClosed) {
            if(!scene_manager_search_and_switch_to_previous_scene(
                   subghz->scene_manager, SubGhzSceneBfSetupAttack)) {
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneBfStart);
            }
            return true;
        }
    }

    return false;
}

void subghz_scene_bf_save_success_on_exit(void* context) {
    furi_assert(context);

    SubGhz* subghz = (SubGhz*)context;

    // Clear view
    Popup* popup = subghz->popup;
    popup_set_header(popup, NULL, 0, 0, AlignCenter, AlignBottom);
    popup_set_text(popup, NULL, 0, 0, AlignCenter, AlignTop);
    popup_set_icon(popup, 0, 0, NULL);
    popup_set_callback(popup, NULL);
    popup_set_context(popup, NULL);
    popup_set_timeout(popup, 0);
    popup_disable_timeout(popup);
}
