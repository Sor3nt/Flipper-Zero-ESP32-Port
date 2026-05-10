#include "wlan_app.h"
#include "wlan_hal.h"
#include "wlan_netcut.h"

static bool wlan_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    WlanApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool wlan_app_back_event_callback(void* context) {
    furi_assert(context);
    WlanApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void wlan_app_tick_event_callback(void* context) {
    furi_assert(context);
    WlanApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

static WlanApp* wlan_app_alloc(void) {
    WlanApp* app = malloc(sizeof(WlanApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->scene_manager = scene_manager_alloc(&wlan_app_scene_handlers, app);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, wlan_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, wlan_app_back_event_callback);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, wlan_app_tick_event_callback, 250);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, WlanAppViewSubmenu, submenu_get_view(app->submenu));

    app->widget = widget_alloc();
    view_dispatcher_add_view(app->view_dispatcher, WlanAppViewWidget, widget_get_view(app->widget));

    app->loading = loading_alloc();
    view_dispatcher_add_view(app->view_dispatcher, WlanAppViewLoading, loading_get_view(app->loading));

    app->popup = popup_alloc();
    view_dispatcher_add_view(app->view_dispatcher, WlanAppViewPopup, popup_get_view(app->popup));

    app->text_input = text_input_alloc();
    view_dispatcher_add_view(app->view_dispatcher, WlanAppViewTextInput, text_input_get_view(app->text_input));

    app->variable_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        WlanAppViewVariableItemList,
        variable_item_list_get_view(app->variable_item_list));

    app->view_lan = wlan_lan_view_alloc();
    wlan_lan_view_set_view_dispatcher(app->view_lan, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, WlanAppViewLan, app->view_lan);

    app->view_connect = wlan_connect_view_alloc();
    wlan_connect_view_set_view_dispatcher(app->view_connect, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, WlanAppViewConnect, app->view_connect);

    app->view_portscan = wlan_portscan_view_alloc();
    view_set_context(app->view_portscan, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, WlanAppViewPortscan, app->view_portscan);

    app->view_handshake = wlan_handshake_view_alloc();
    view_set_context(app->view_handshake, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, WlanAppViewHandshake, app->view_handshake);

    app->view_handshake_channel = wlan_handshake_channel_view_alloc();
    view_set_context(app->view_handshake_channel, app->view_dispatcher);
    view_dispatcher_add_view(
        app->view_dispatcher, WlanAppViewHandshakeChannel, app->view_handshake_channel);

    app->view_deauther = wlan_deauther_view_alloc();
    view_set_context(app->view_deauther, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, WlanAppViewDeauther, app->view_deauther);

    app->sniffer_view_obj = wlan_sniffer_view_alloc();
    app->view_sniffer = wlan_sniffer_view_get_view(app->sniffer_view_obj);
    view_dispatcher_add_view(app->view_dispatcher, WlanAppViewSniffer, app->view_sniffer);

    app->evil_portal_view_obj = wlan_evil_portal_view_alloc();
    app->view_evil_portal = wlan_evil_portal_view_get_view(app->evil_portal_view_obj);
    view_dispatcher_add_view(app->view_dispatcher, WlanAppViewEvilPortal, app->view_evil_portal);

    app->evil_portal_captured_view_obj = wlan_evil_portal_captured_view_alloc();
    app->view_evil_portal_captured =
        wlan_evil_portal_captured_view_get_view(app->evil_portal_captured_view_obj);
    view_dispatcher_add_view(
        app->view_dispatcher, WlanAppViewEvilPortalCaptured, app->view_evil_portal_captured);


    app->ap_records = malloc(sizeof(WlanApRecord) * WLAN_APP_MAX_APS);
    app->ap_count = 0;
    app->ap_selected_index = 0;

    app->devices = malloc(sizeof(WlanDeviceRecord) * WLAN_APP_MAX_DEVICES);
    app->device_count = 0;
    app->device_selected_index = 0;
    app->lan_menu_device_idx = -1;
    app->lan_popup_active = false;
    app->lan_scan_complete = false;

    memset(app->deauth_clients, 0, sizeof(app->deauth_clients));
    app->deauth_client_count = 0;
    app->deauth_auto = false;

    strcpy(app->evil_portal_ssid, "Free WiFi");
    app->evil_portal_channel = 6;
    app->evil_portal_template_index = 0;
    app->evil_portal_valid_ssid[0] = '\0';
    app->evil_portal_valid_pwd[0] = '\0';

    app->connected = false;
    app->target_selected = false;
    memset(&app->connected_ap, 0, sizeof(app->connected_ap));
    memset(&app->target_ap, 0, sizeof(app->target_ap));
    memset(app->password_input, 0, sizeof(app->password_input));

    app->attack_block_internet = false;
    app->attack_throttle = WlanAppThrottleOff;

    app->text_buf = furi_string_alloc();
    app->netcut = wlan_netcut_alloc();

    wlan_handshake_settings_load(&app->hs_settings);

    return app;
}

static void wlan_app_free(WlanApp* app) {
    // Aktive ARP-Spoofs beenden + Restore-Frames senden, dann WiFi-Stack stoppen.
    if(app->netcut) {
        wlan_netcut_free(app->netcut);
        app->netcut = NULL;
    }
    wlan_hal_stop();

    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewWidget);
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewLoading);
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewPopup);
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewVariableItemList);
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewLan);
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewConnect);
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewPortscan);
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewHandshake);
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewHandshakeChannel);
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewDeauther);
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewSniffer);
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewEvilPortal);
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewEvilPortalCaptured);

    submenu_free(app->submenu);
    widget_free(app->widget);
    loading_free(app->loading);
    popup_free(app->popup);
    text_input_free(app->text_input);
    variable_item_list_free(app->variable_item_list);
    wlan_lan_view_free(app->view_lan);
    wlan_connect_view_free(app->view_connect);
    wlan_portscan_view_free(app->view_portscan);
    wlan_handshake_view_free(app->view_handshake);
    wlan_handshake_channel_view_free(app->view_handshake_channel);
    wlan_deauther_view_free(app->view_deauther);
    wlan_sniffer_view_free(app->sniffer_view_obj);
    wlan_evil_portal_view_free(app->evil_portal_view_obj);
    wlan_evil_portal_captured_view_free(app->evil_portal_captured_view_obj);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    free(app->ap_records);
    free(app->devices);
    furi_string_free(app->text_buf);

    furi_record_close(RECORD_GUI);
    app->gui = NULL;
    free(app);
}

int32_t wlan_app(void* args) {
    UNUSED(args);
    WlanApp* app = wlan_app_alloc();
    scene_manager_next_scene(app->scene_manager, WlanAppSceneMain);
    view_dispatcher_run(app->view_dispatcher);
    wlan_app_free(app);
    return 0;
}
