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
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdSpectrum);
    subghz_spectrum_start(subghz->subghz_spectrum);
}

bool subghz_scene_spectrum_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventViewSpectrumBack) {
            scene_manager_previous_scene(subghz->scene_manager);
            return true;
        }
    }
    return false;
}

void subghz_scene_spectrum_on_exit(void* context) {
    SubGhz* subghz = context;
    subghz_spectrum_stop(subghz->subghz_spectrum);
}
