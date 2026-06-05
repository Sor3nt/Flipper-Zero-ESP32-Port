#include "nrf24_app.h"
#include "helpers/nrf24_jam_config.h"
#include "helpers/nrf24_channel_source.h"
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
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->storage = furi_record_open(RECORD_STORAGE);

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

    app->var_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        Nrf24ViewJamConfig,
        variable_item_list_get_view(app->var_item_list));

    app->spectrum_view = nrf24_spectrum_view_alloc();
    view_set_context(app->spectrum_view, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, Nrf24ViewSpectrum, app->spectrum_view);

    app->jam_view = nrf24_jam_view_alloc();
    view_set_context(app->jam_view, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, Nrf24ViewJam, app->jam_view);

    app->scan_view = nrf24_scan_view_alloc();
    view_set_context(app->scan_view, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, Nrf24ViewScan, app->scan_view);

    app->mj_scan_view = nrf24_mj_scan_view_alloc();
    view_set_context(app->mj_scan_view, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, Nrf24ViewMjScan, app->mj_scan_view);

    app->mj_attack_view = nrf24_mj_attack_view_alloc();
    view_set_context(app->mj_attack_view, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, Nrf24ViewMjAttack, app->mj_attack_view);

    app->wifi_aps = NULL;
    app->wifi_ap_count = 0;
    app->selected_wifi_ssid[0] = '\0';
    app->selected_wifi_channel = 0;

    nrf24_jam_state_init(&app->jam);
    nrf24_jam_config_load();

    app->mj_target_count = 0;
    app->mj_selected_target = -1;
    app->mj_script_path = furi_string_alloc();
    app->mj_auto_mode = false;

    return app;
}

static void nrf24_app_free(Nrf24App* app) {
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewWidget);
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewJamConfig);
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewSpectrum);
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewJam);
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewScan);
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewMjScan);
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewMjAttack);

    submenu_free(app->submenu);
    widget_free(app->widget);
    variable_item_list_free(app->var_item_list);
    nrf24_spectrum_view_free(app->spectrum_view);
    nrf24_jam_view_free(app->jam_view);
    nrf24_scan_view_free(app->scan_view);
    nrf24_mj_scan_view_free(app->mj_scan_view);
    nrf24_mj_attack_view_free(app->mj_attack_view);

    if(app->wifi_aps) {
        free(app->wifi_aps);
        app->wifi_aps = NULL;
    }

    if(app->mj_script_path) {
        furi_string_free(app->mj_script_path);
        app->mj_script_path = NULL;
    }

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_STORAGE);
    app->storage = NULL;
    furi_record_close(RECORD_DIALOGS);
    app->dialogs = NULL;
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
