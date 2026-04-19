#include "wifi_app.h"
#include "wifi_hal.h"
#include "views/ap_list.h"
#include "views/deauther_view.h"
#include "views/sniffer_view.h"
#include "views/crawler_view.h"
#include "views/handshake_view.h"
#include "views/handshake_channel_view.h"
#include "views/airsnitch_view.h"
#include "views/netscan_view.h"
#include "views/beacon_view.h"

static bool wifi_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    WifiApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool wifi_app_back_event_callback(void* context) {
    furi_assert(context);
    WifiApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void wifi_app_tick_event_callback(void* context) {
    furi_assert(context);
    WifiApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

static WifiApp* wifi_app_alloc(void) {
    WifiApp* app = malloc(sizeof(WifiApp));
    app->gui = furi_record_open(RECORD_GUI);
    app->scene_manager = scene_manager_alloc(&wifi_app_scene_handlers, app);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, wifi_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, wifi_app_back_event_callback);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, wifi_app_tick_event_callback, 250);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    app->submenu = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, WifiAppViewSubmenu, submenu_get_view(app->submenu));
    app->widget = widget_alloc();
    view_dispatcher_add_view(app->view_dispatcher, WifiAppViewWidget, widget_get_view(app->widget));
    app->loading = loading_alloc();
    view_dispatcher_add_view(app->view_dispatcher, WifiAppViewLoading, loading_get_view(app->loading));
    app->view_ap_list = ap_list_alloc();
    ap_list_set_view_dispatcher(app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, WifiAppViewApList, app->view_ap_list);
    app->view_deauther = deauther_view_alloc();
    view_set_context(app->view_deauther, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, WifiAppViewDeauther, app->view_deauther);
    app->view_sniffer = sniffer_view_alloc();
    view_set_context(app->view_sniffer, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, WifiAppViewSniffer, app->view_sniffer);
    app->text_input = text_input_alloc();
    view_dispatcher_add_view(app->view_dispatcher, WifiAppViewTextInput, text_input_get_view(app->text_input));
    app->view_crawler = crawler_view_alloc();
    crawler_view_set_view_dispatcher(app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, WifiAppViewCrawler, app->view_crawler);
    app->view_handshake = handshake_view_alloc();
    view_set_context(app->view_handshake, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, WifiAppViewHandshake, app->view_handshake);
    app->view_handshake_channel = handshake_channel_view_alloc();
    view_set_context(app->view_handshake_channel, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, WifiAppViewHandshakeChannel, app->view_handshake_channel);
    app->view_airsnitch = airsnitch_view_alloc();
    view_set_context(app->view_airsnitch, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, WifiAppViewAirSnitch, app->view_airsnitch);
    app->view_netscan = netscan_view_alloc();
    view_set_context(app->view_netscan, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, WifiAppViewNetscan, app->view_netscan);

    // Beacon view allocation
    app->beacon_view_obj = beacon_view_alloc();
    app->view_beacon = beacon_view_get_view(app->beacon_view_obj);
    view_dispatcher_add_view(app->view_dispatcher, WifiAppViewBeacon, app->view_beacon);

    app->ap_records = malloc(sizeof(WifiApRecord) * WIFI_APP_MAX_APS);
    app->ap_count = 0;
    app->selected_index = 0;
    app->text_buf = furi_string_alloc();
    app->deauth_running = false;
    app->deauth_frame_count = 0;
    app->sniffer_running = false;
    app->sniffer_pkt_count = 0;
    app->sniffer_bytes = 0;
    app->sniffer_channel = 1;
    app->handshake_running = false;
    app->handshake_deauth_running = false;
    app->handshake_eapol_count = 0;
    app->handshake_deauth_count = 0;
    app->handshake_complete = false;
    app->scanner_next_scene = WifiAppSceneApDetail;
    memset(app->crawler_domain, 0, sizeof(app->crawler_domain));
    memset(&app->crawler_state, 0, sizeof(app->crawler_state));
    memset(app->single_ssid, 0, sizeof(app->single_ssid));
    return app;
}

static void wifi_app_free(WifiApp* app) {
    if(wifi_hal_is_connected()) {
        wifi_hal_cleanup_keep_connection();
    } else {
        wifi_hal_cleanup();
    }
    view_dispatcher_remove_view(app->view_dispatcher, WifiAppViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, WifiAppViewWidget);
    view_dispatcher_remove_view(app->view_dispatcher, WifiAppViewLoading);
    view_dispatcher_remove_view(app->view_dispatcher, WifiAppViewApList);
    view_dispatcher_remove_view(app->view_dispatcher, WifiAppViewDeauther);
    view_dispatcher_remove_view(app->view_dispatcher, WifiAppViewSniffer);
    view_dispatcher_remove_view(app->view_dispatcher, WifiAppViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, WifiAppViewCrawler);
    view_dispatcher_remove_view(app->view_dispatcher, WifiAppViewHandshake);
    view_dispatcher_remove_view(app->view_dispatcher, WifiAppViewHandshakeChannel);
    view_dispatcher_remove_view(app->view_dispatcher, WifiAppViewAirSnitch);
    view_dispatcher_remove_view(app->view_dispatcher, WifiAppViewNetscan);
    view_dispatcher_remove_view(app->view_dispatcher, WifiAppViewBeacon);
    submenu_free(app->submenu);
    widget_free(app->widget);
    loading_free(app->loading);
    text_input_free(app->text_input);
    ap_list_free(app->view_ap_list);
    deauther_view_free(app->view_deauther);
    sniffer_view_free(app->view_sniffer);
    crawler_view_free(app->view_crawler);
    handshake_view_free(app->view_handshake);
    handshake_channel_view_free(app->view_handshake_channel);
    airsnitch_view_free(app->view_airsnitch);
    netscan_view_free(app->view_netscan);
    beacon_view_free(app->beacon_view_obj);
    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);
    free(app->ap_records);
    furi_string_free(app->text_buf);
    furi_record_close(RECORD_GUI);
    app->gui = NULL;
    free(app);
}

int32_t wifi_app(void* args) {
    WifiApp* app = wifi_app_alloc();
    wifi_hal_preinit();
    wifi_hal_start();
    if(args && strcmp((const char*)args, "handshake_channel") == 0) {
        scene_manager_next_scene(app->scene_manager, WifiAppSceneHandshakeChannel);
    } else if(args && strcmp((const char*)args, "channel_deauth") == 0) {
        app->deauth_mode = WifiAppDeauthModeChannel;
        scene_manager_next_scene(app->scene_manager, WifiAppSceneDeauther);
    } else {
        scene_manager_next_scene(app->scene_manager, WifiAppSceneMenu);
    }
    view_dispatcher_run(app->view_dispatcher);
    wifi_app_free(app);
    return 0;
}