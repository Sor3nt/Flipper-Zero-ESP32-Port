#include "../nrf24_app.h"

void nrf24_app_scene_spectrum_on_enter(void* context) {
    Nrf24App* app = context;
    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewSpectrum);
    nrf24_spectrum_start(app->spectrum);
}

bool nrf24_app_scene_spectrum_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void nrf24_app_scene_spectrum_on_exit(void* context) {
    Nrf24App* app = context;
    nrf24_spectrum_stop(app->spectrum);
}
