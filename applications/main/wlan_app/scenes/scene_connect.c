#include "../wlan_app.h"
#include "../wlan_hal.h"
#include "../wlan_passwords.h"

#define CONNECT_MENU_HANDSHAKE 1
#define CONNECT_MENU_DEAUTH    2
#define CONNECT_MENU_SNIFFER   3
#define CONNECT_MENU_PORTAL    4

static void connect_open_menu_for_selected(WlanApp* app) {
    uint8_t sel = wlan_connect_view_get_selected(app->view_connect);
    if(sel >= app->ap_count) return;
    app->ap_selected_index = sel;

    wlan_connect_view_clear_menu(app->view_connect);
    wlan_connect_view_add_menu_item(app->view_connect, "Handshake", CONNECT_MENU_HANDSHAKE);
    wlan_connect_view_add_menu_item(app->view_connect, "Deauth", CONNECT_MENU_DEAUTH);
    wlan_connect_view_add_menu_item(app->view_connect, "Sniffer", CONNECT_MENU_SNIFFER);
    wlan_connect_view_add_menu_item(app->view_connect, "Evil Portal", CONNECT_MENU_PORTAL);
    wlan_connect_view_open_menu(app->view_connect);
}

static void wlan_app_scene_connect_run_scan(WlanApp* app) {
    app->ap_count = 0;

    if(!wlan_hal_is_started()) {
        if(!wlan_hal_start()) {
            return;
        }
    }

    wifi_ap_record_t* raw = NULL;
    uint16_t found = 0;
    wlan_hal_scan(&raw, &found, WLAN_APP_MAX_APS);

    for(uint16_t i = 0; i < found; ++i) {
        WlanApRecord* r = &app->ap_records[app->ap_count++];
        memset(r, 0, sizeof(*r));
        strncpy(r->ssid, (const char*)raw[i].ssid, sizeof(r->ssid) - 1);
        memcpy(r->bssid, raw[i].bssid, 6);
        r->rssi = raw[i].rssi;
        r->channel = raw[i].primary;
        r->authmode = raw[i].authmode;
        r->is_open = (raw[i].authmode == WIFI_AUTH_OPEN);
        r->has_password = !r->is_open && wlan_password_exists(r->ssid);
    }
    if(raw) free(raw);
}

void wlan_app_scene_connect_on_enter(void* context) {
    WlanApp* app = context;

    // Während des Scans Loading-View zeigen.
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewLoading);

    wlan_app_scene_connect_run_scan(app);

    wlan_connect_view_clear(app->view_connect);
    for(uint16_t i = 0; i < app->ap_count; ++i) {
        WlanApRecord* r = &app->ap_records[i];
        bool unlocked = r->is_open || r->has_password;
        wlan_connect_view_add_ap(app->view_connect, r->ssid, unlocked, i);
    }

    uint8_t restore = scene_manager_get_scene_state(app->scene_manager, WlanAppSceneConnect);
    wlan_connect_view_set_selected(app->view_connect, restore);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewConnect);
}

bool wlan_app_scene_connect_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom &&
       event.event == WlanAppCustomEventApSelected) {
        uint8_t sel = wlan_connect_view_get_selected(app->view_connect);
        if(sel < app->ap_count) {
            app->ap_selected_index = sel;
            scene_manager_set_scene_state(app->scene_manager, WlanAppSceneConnect, sel);
            scene_manager_next_scene(app->scene_manager, WlanAppSceneSsidScreen);
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeCustom &&
              event.event == WlanAppCustomEventConnectLongOk) {
        connect_open_menu_for_selected(app);
        consumed = true;
    } else if(event.type == SceneManagerEventTypeCustom &&
              event.event == WlanAppCustomEventConnectMenuOk) {
        uint8_t mi = wlan_connect_view_get_menu_selected(app->view_connect);
        WlanConnectMenuItem mit = wlan_connect_view_get_menu_item(app->view_connect, mi);
        wlan_connect_view_close_menu(app->view_connect);

        if(app->ap_selected_index < app->ap_count) {
            // SSID als Target merken (analog SSID-Screen "Attack").
            memcpy(&app->target_ap, &app->ap_records[app->ap_selected_index],
                sizeof(WlanApRecord));
            app->target_selected = true;
        }

        switch(mit.user_id) {
        case CONNECT_MENU_HANDSHAKE:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneHandshake);
            break;
        case CONNECT_MENU_DEAUTH:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneNetworkDeauth);
            break;
        case CONNECT_MENU_SNIFFER:
            scene_manager_next_scene(app->scene_manager, WlanAppScenePackageSniffer);
            break;
        case CONNECT_MENU_PORTAL:
            // Evil-Portal-Settings mit SSID/Channel des selektierten WLANs vorbelegen.
            if(app->target_ap.ssid[0]) {
                strncpy(app->evil_portal_ssid, app->target_ap.ssid,
                    sizeof(app->evil_portal_ssid) - 1);
                app->evil_portal_ssid[sizeof(app->evil_portal_ssid) - 1] = '\0';
            }
            if(app->target_ap.channel) {
                app->evil_portal_channel = app->target_ap.channel;
            }
            scene_manager_next_scene(app->scene_manager, WlanAppSceneEvilPortalMenu);
            break;
        }
        consumed = true;
    }

    return consumed;
}

void wlan_app_scene_connect_on_exit(void* context) {
    WlanApp* app = context;
    wlan_connect_view_clear(app->view_connect);
}
