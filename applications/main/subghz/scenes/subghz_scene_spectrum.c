#include "../subghz_i.h" // IWYU pragma: keep
#include "../views/subghz_spectrum.h"

void subghz_scene_spectrum_callback(SubGhzCustomEvent event, void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

void subghz_scene_spectrum_on_enter(void* context) {
    SubGhz* subghz = context;
    subghz_spectrum_set_callback(subghz->subghz_spectrum, subghz_scene_spectrum_callback, subghz);
    subghz_spectrum_set_start_frequency(
        subghz->subghz_spectrum, subghz->last_settings->frequency);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdSpectrum);
    notification_message(subghz->notifications, &sequence_display_backlight_enforce_on);
    if(!subghz_spectrum_start(subghz->subghz_spectrum)) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventViewSpectrumStartError);
    }
}

bool subghz_scene_spectrum_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventViewSpectrumBack) {
            scene_manager_previous_scene(subghz->scene_manager);
            return true;
        } else if(event.event == SubGhzCustomEventViewSpectrumStartError) {
            furi_string_set(subghz->error_str, "Unable to start\nRF Spectrum.");
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowErrorSub);
            return true;
        }
    }
    return false;
}

void subghz_scene_spectrum_on_exit(void* context) {
    SubGhz* subghz = context;
    subghz_spectrum_stop(subghz->subghz_spectrum);
    notification_message(subghz->notifications, &sequence_display_backlight_enforce_auto);
}
