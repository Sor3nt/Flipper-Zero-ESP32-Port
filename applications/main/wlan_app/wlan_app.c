#include "wlan_app.h"
#include "wlan_html_inject.h"
#include "wlan_cred_sniff.h"
#include <wlan_hal.h>
#include "wlan_netcut.h"
#include <wifi.h>
#include <wlan_passwords.h>

#include <esp_heap_caps.h> /* TEMP: heap diagnostics for the C6 OOM crash */

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

    app->live_creds_view_obj = wlan_live_creds_view_alloc();
    app->view_live_creds = wlan_live_creds_view_get_view(app->live_creds_view_obj);
    view_dispatcher_add_view(app->view_dispatcher, WlanAppViewLiveCreds, app->view_live_creds);

    app->view_sd_update = wlan_sd_update_view_alloc();
    view_set_context(app->view_sd_update, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, WlanAppViewSdUpdate, app->view_sd_update);

    app->view_fw_update = wlan_fw_update_view_alloc();
    view_set_context(app->view_fw_update, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, WlanAppViewFwUpdate, app->view_fw_update);

    app->view_androidtv_remote = wlan_androidtv_remote_view_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, WlanAppViewAndroidTvRemote, app->view_androidtv_remote);


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
    app->evil_portal_templates.count = 0;
    app->evil_portal_valid_ssid[0] = '\0';
    app->evil_portal_valid_pwd[0] = '\0';

    app->connected = false;
    app->target_selected = false;
    memset(&app->connected_ap, 0, sizeof(app->connected_ap));
    memset(&app->target_ap, 0, sizeof(app->target_ap));
    memset(app->password_input, 0, sizeof(app->password_input));

    /* Globales WiFi bleibt über App-Grenzen verbunden. Ist die STA bereits
     * verbunden (z.B. beim erneuten Öffnen der App), den UI-Zustand aus der
     * laufenden Verbindung rekonstruieren, statt „Select Wifi" zu zeigen. */
    wifi_ap_record_t cur_ap;
    if(wlan_hal_is_connected() && wlan_hal_get_connected_ap(&cur_ap)) {
        app->connected = true;
        strncpy(
            app->connected_ap.ssid,
            (const char*)cur_ap.ssid,
            sizeof(app->connected_ap.ssid) - 1);
        memcpy(app->connected_ap.bssid, cur_ap.bssid, 6);
        app->connected_ap.rssi = cur_ap.rssi;
        app->connected_ap.channel = cur_ap.primary;
        app->connected_ap.authmode = cur_ap.authmode;
        app->connected_ap.is_open = (cur_ap.authmode == WIFI_AUTH_OPEN);
        app->connected_ap.has_password = wlan_password_exists(app->connected_ap.ssid);
    }

    app->attack_block_internet = false;
    app->attack_throttle = WlanAppThrottleOff;

    app->sd_update = wlan_sd_update_alloc();
    app->fw_update = wlan_fw_update_alloc();
    // FW-Marker /ext/.fw_version auf die laufende Version (FURI_ESP32_VERSION)
    // ziehen — schreibt nur, wenn er fehlt oder abweicht.
    wlan_fw_update_sync_marker();

    // SMB Browser: lazily allocated on first use (allocates PSRAM buffers +
    // a worker task), freed in wlan_app_free.
    app->smb = NULL;
    app->smb_server_ip[0] = '\0';
    app->smb_server_name[0] = '\0';
    app->smb_user[0] = '\0';
    app->smb_pass[0] = '\0';
    app->smb_share[0] = '\0';
    app->smb_path[0] = '\0';

    // Android TV remote: lazily allocated on first use (allocates PSRAM buffers
    // + a worker task with an internal-DRAM stack), freed in wlan_app_free.
    app->androidtv = NULL;
    app->androidtv_ip[0] = '\0';
    app->androidtv_name[0] = '\0';
    app->androidtv_pin[0] = '\0';

    app->text_buf = furi_string_alloc();
    app->netcut = wlan_netcut_alloc();
    /* TEMP: log heap right before the 15KB cred_sniff alloc that OOM-crashed. */
    printf(
        "[WLAN] heap before cred_sniff: free=%u largest_block=%u\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    app->cred_sniff = wlan_cred_sniff_alloc();
    printf("[WLAN] cred_sniff=%p\n", (void*)app->cred_sniff);
    wlan_netcut_set_cred_sniff(app->netcut, app->cred_sniff);
    wlan_html_inject_set_cred_sniff(app->cred_sniff);

    app->mitm_inject_enabled = true;
    app->mitm_store_cred = true;
    // Default-Payload für "custom". Wird raw als JS am mitm-server /code
    // ausgeliefert; in HTML injizieren wir nur einen <script src=...>-Loader.
    strcpy(app->mitm_inject_code, "alert(1234);");
    app->mitm_payloads.count = 0;
    app->mitm_payload_index = 0; // wird in scene_mitm_menu_on_enter auf "custom" gesetzt

    wlan_handshake_settings_load(&app->hs_settings);

    return app;
}

static void wlan_app_free(WlanApp* app) {
    // Aktive ARP-Spoofs beenden + Restore-Frames senden (deinstalliert auch den
    // L2-Hook, der den Cred-Sniffer referenziert), dann erst cred_sniff freigeben.
    if(app->netcut) {
        wlan_netcut_free(app->netcut);
        app->netcut = NULL;
    }
    if(app->cred_sniff) {
        wlan_cred_sniff_free(app->cred_sniff);
        app->cred_sniff = NULL;
    }
    if(app->sd_update) {
        wlan_sd_update_free(app->sd_update);
        app->sd_update = NULL;
    }
    if(app->fw_update) {
        wlan_fw_update_free(app->fw_update);
        app->fw_update = NULL;
    }
    if(app->smb) {
        wlan_smb_free(app->smb);
        app->smb = NULL;
    }
    if(app->androidtv) {
        wlan_androidtv_free(app->androidtv);
        app->androidtv = NULL;
    }
    /* WiFi global aktiv (Lock-Menü „Enable WiFi" oder erfolgreicher STA-Connect)
     * → Radio + Verbindung beim App-Verlassen NICHT abbauen, damit man verbunden
     * bleibt. Nur transiente WiFi-Nutzung (Scan/Attack ohne globales WiFi) wird
     * hier abgeräumt (stellt via wlan_hal ggf. BT wieder her). */
    Wifi* wifi = furi_record_open(RECORD_WIFI);
    bool wifi_global = wifi_is_enabled(wifi);
    furi_record_close(RECORD_WIFI);
    if(!wifi_global) {
        wlan_hal_stop();
    }

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
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewLiveCreds);
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewSdUpdate);
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewFwUpdate);
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewAndroidTvRemote);

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
    wlan_live_creds_view_free(app->live_creds_view_obj);
    wlan_sd_update_view_free(app->view_sd_update);
    wlan_fw_update_view_free(app->view_fw_update);
    wlan_androidtv_remote_view_free(app->view_androidtv_remote);

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
    WlanApp* app = wlan_app_alloc();

    // Launch arg "webfs" (e.g. from the desktop lock menu) opens the
    // Web-Filesystem flow directly: info scene if already connected, else the
    // Select Wifi / Dedicated AP menu.
    const char* arg = args;
    if(arg && strcmp(arg, "webfs") == 0) {
        if(wlan_hal_is_connected()) {
            scene_manager_set_scene_state(app->scene_manager, WlanAppSceneWebFsInfo, 0 /* STA */);
            scene_manager_next_scene(app->scene_manager, WlanAppSceneWebFsInfo);
        } else {
            scene_manager_next_scene(app->scene_manager, WlanAppSceneWebFsMenu);
        }
    } else if(arg && strcmp(arg, "update") == 0) {
        // "Update Firmware" aus dem Settings-Menü: kombiniertes Update (FW + SD)
        // direkt öffnen. Bereits verbunden → direkt zur Update-Scene, sonst erst
        // Connect-Flow (fw_update_flow routet nach Erfolg zur Update-Scene).
        app->fw_update_flow = true;
        if(wlan_hal_is_connected()) {
            scene_manager_next_scene(app->scene_manager, WlanAppSceneFwUpdate);
        } else {
            scene_manager_next_scene(app->scene_manager, WlanAppSceneConnect);
        }
    } else {
        scene_manager_next_scene(app->scene_manager, WlanAppSceneMain);
    }

    view_dispatcher_run(app->view_dispatcher);
    wlan_app_free(app);
    return 0;
}
