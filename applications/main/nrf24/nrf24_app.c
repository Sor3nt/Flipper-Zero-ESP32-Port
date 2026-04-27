#include "nrf24_app.h"
#include <stdlib.h>

static bool nrf24_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    Nrf24App* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool nrf24_app_back_event_callback(void* context) {
    furi_assert(context);
    Nrf24App* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void nrf24_app_tick_event_callback(void* context) {
    furi_assert(context);
    Nrf24App* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

static Nrf24App* nrf24_app_alloc(void) {
    Nrf24App* app = malloc(sizeof(Nrf24App));

    app->gui = furi_record_open(RECORD_GUI);

    app->scene_manager = scene_manager_alloc(&nrf24_app_scene_handlers, app);
    app->view_dispatcher = view_dispatcher_alloc();

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, nrf24_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, nrf24_app_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, nrf24_app_tick_event_callback, 100);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, Nrf24ViewSubmenu, submenu_get_view(app->submenu));

    app->widget = widget_alloc();
    view_dispatcher_add_view(app->view_dispatcher, Nrf24ViewWidget, widget_get_view(app->widget));

    app->spectrum_view = nrf24_spectrum_view_alloc();
    view_set_context(app->spectrum_view, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, Nrf24ViewSpectrum, app->spectrum_view);

    app->ch_jammer_view = nrf24_ch_jammer_view_alloc();
    view_set_context(app->ch_jammer_view, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, Nrf24ViewChJammer, app->ch_jammer_view);

    app->wifi_jam_view = nrf24_wifi_jam_view_alloc();
    view_set_context(app->wifi_jam_view, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, Nrf24ViewWifiJam, app->wifi_jam_view);

    app->wifi_aps = NULL;
    app->wifi_ap_count = 0;
    app->selected_wifi_ssid[0] = '\0';
    app->selected_wifi_channel = 0;

    return app;
}

static void nrf24_app_free(Nrf24App* app) {
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewWidget);
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewSpectrum);
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewChJammer);
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewWifiJam);

    submenu_free(app->submenu);
    widget_free(app->widget);
    nrf24_spectrum_view_free(app->spectrum_view);
    nrf24_ch_jammer_view_free(app->ch_jammer_view);
    nrf24_wifi_jam_view_free(app->wifi_jam_view);

    if(app->wifi_aps) {
        free(app->wifi_aps);
        app->wifi_aps = NULL;
    }

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);
    app->gui = NULL;

    free(app);
}

int32_t nrf24_app(void* args) {
    UNUSED(args);

    Nrf24App* app = nrf24_app_alloc();

    scene_manager_next_scene(app->scene_manager, Nrf24AppSceneMenu);
    view_dispatcher_run(app->view_dispatcher);

    nrf24_app_free(app);
    return 0;
}
