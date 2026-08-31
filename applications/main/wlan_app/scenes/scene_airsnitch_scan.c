#include "../wlan_app.h"
#include <wlan_hal.h>

// AirSnitch-Schritt 1: WiFi-Scan, damit der User das Zielnetz wählt. Reine
// Beschriftung — wir verbinden uns NICHT mit dem Ziel (siehe scene_airsnitch_probe).

static void airsnitch_scan_submenu_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void airsnitch_scan_run(WlanApp* app) {
    app->ap_count = 0;

    if(!wlan_hal_is_started()) {
        if(!wlan_hal_start()) return;
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
    }
    if(raw) free(raw);
}

static void airsnitch_set_target(WlanApp* app, const char* ssid) {
    strncpy(app->airsnitch_target_ssid, ssid, sizeof(app->airsnitch_target_ssid) - 1);
    app->airsnitch_target_ssid[sizeof(app->airsnitch_target_ssid) - 1] = '\0';
}

void wlan_app_scene_airsnitch_scan_on_enter(void* context) {
    WlanApp* app = context;

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewLoading);
    airsnitch_scan_run(app);

    // Scan leer (sollte selten sein, da verbunden) → aktuelles Netz als
    // einzigen Eintrag synthetisieren, damit der normale Auswahl-Pfad greift.
    if(app->ap_count == 0 && app->connected) {
        memcpy(&app->ap_records[0], &app->connected_ap, sizeof(WlanApRecord));
        app->ap_count = 1;
    }

    submenu_reset(app->submenu);
    submenu_set_header_centered(app->submenu, "AirSnitch: Target");

    for(uint16_t i = 0; i < app->ap_count; ++i) {
        WlanApRecord* r = &app->ap_records[i];
        bool current = app->connected &&
            strncmp(r->ssid, app->connected_ap.ssid, sizeof(r->ssid)) == 0;
        char label[48];
        snprintf(label, sizeof(label), "%s%s", r->ssid[0] ? r->ssid : "(hidden)",
            current ? " *" : "");
        submenu_add_item(app->submenu, label, i, airsnitch_scan_submenu_cb, app);
    }

    uint8_t restore =
        scene_manager_get_scene_state(app->scene_manager, WlanAppSceneAirSnitchScan);
    submenu_set_selected_item(app->submenu, restore);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSubmenu);
}

bool wlan_app_scene_airsnitch_scan_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        uint32_t idx = event.event;
        if(idx < app->ap_count) {
            scene_manager_set_scene_state(app->scene_manager, WlanAppSceneAirSnitchScan, idx);
            airsnitch_set_target(app, app->ap_records[idx].ssid);
            scene_manager_next_scene(app->scene_manager, WlanAppSceneAirSnitchProbe);
            consumed = true;
        }
    }

    return consumed;
}

void wlan_app_scene_airsnitch_scan_on_exit(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
}
