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
    if(!subghz_spectrum_start(subghz->subghz_spectrum)) {
        DialogMessage* message = dialog_message_alloc();
        dialog_message_set_header(message, "Spectrum unavailable", 64, 8, AlignCenter, AlignTop);
        dialog_message_set_text(message, "Not enough memory", 64, 32, AlignCenter, AlignCenter);
        dialog_message_set_buttons(message, "Back", NULL, NULL);
        dialog_message_show(subghz->dialogs, message);
        dialog_message_free(message);
        view_dispatcher_send_custom_event(subghz->view_dispatcher, SubGhzCustomEventViewSpectrumBack);
    }
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
