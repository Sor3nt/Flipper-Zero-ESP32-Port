#include "../wlan_app.h"

enum NetworkActionsIndex {
    NaIndexHandshake,
    NaIndexDeauth,
    NaIndexSniffer,
    NaIndexEvilPortal,
    NaIndexSmb,
    NaIndexAirSnitch,
    NaIndexAndroidTv,
};

static void network_actions_submenu_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wlan_app_scene_network_actions_on_enter(void* context) {
    WlanApp* app = context;

    submenu_reset(app->submenu);
    const char* ssid =
        app->target_ap.ssid[0] ? app->target_ap.ssid :
        (app->connected ? app->connected_ap.ssid : NULL);
    char header[48];
    if(ssid) {
        snprintf(header, sizeof(header), "%s", ssid);
        submenu_set_header(app->submenu, header);
    } else {
        submenu_set_header(app->submenu, "Network Actions");
    }

    submenu_add_item(app->submenu, "Capture Handshake", NaIndexHandshake, network_actions_submenu_cb, app);
    submenu_add_item(app->submenu, "Deauth", NaIndexDeauth, network_actions_submenu_cb, app);
    submenu_add_item(app->submenu, "Package Sniffer", NaIndexSniffer, network_actions_submenu_cb, app);
    submenu_add_item(app->submenu, "Evil Portal", NaIndexEvilPortal, network_actions_submenu_cb, app);
    // SMB Browser needs an active STA connection + the scanned device list.
    if(app->connected) {
        submenu_add_item(app->submenu, "SMB Browser", NaIndexSmb, network_actions_submenu_cb, app);
    }
    // AirSnitch braucht eine aktive STA-Verbindung (Probe vom Gastnetz aus) →
    // nur anzeigen, wenn verbunden.
    if(app->connected) {
        submenu_add_item(app->submenu, "AirSnitch", NaIndexAirSnitch, network_actions_submenu_cb, app);
    }
    // Android TV remote braucht die STA-Verbindung + die gescannte Geräteliste.
    if(app->connected) {
        submenu_add_item(
            app->submenu, "Android TV", NaIndexAndroidTv, network_actions_submenu_cb, app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSubmenu);
}

bool wlan_app_scene_network_actions_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case NaIndexHandshake:
            // Single-Target-Mode: Handshake auf dem ausgewählten/verbundenen AP.
            app->channel_mode_active = false;
            scene_manager_next_scene(app->scene_manager, WlanAppSceneHandshake);
            consumed = true;
            break;
        case NaIndexDeauth:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneNetworkDeauth);
            consumed = true;
            break;
        case NaIndexSniffer:
            scene_manager_next_scene(app->scene_manager, WlanAppScenePackageSniffer);
            consumed = true;
            break;
        case NaIndexEvilPortal: {
            const char* src_ssid =
                app->target_ap.ssid[0] ? app->target_ap.ssid :
                (app->connected ? app->connected_ap.ssid : NULL);
            uint8_t src_channel =
                app->target_ap.channel ? app->target_ap.channel :
                (app->connected ? app->connected_ap.channel : 0);
            if(src_ssid && src_ssid[0]) {
                strncpy(app->evil_portal_ssid, src_ssid,
                    sizeof(app->evil_portal_ssid) - 1);
                app->evil_portal_ssid[sizeof(app->evil_portal_ssid) - 1] = '\0';
            }
            if(src_channel) {
                app->evil_portal_channel = src_channel;
            }
            scene_manager_next_scene(app->scene_manager, WlanAppSceneEvilPortalMenu);
            consumed = true;
            break;
        }
        case NaIndexSmb:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneSmbScan);
            consumed = true;
            break;
        case NaIndexAirSnitch:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneAirSnitchScan);
            consumed = true;
            break;
        case NaIndexAndroidTv:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneAndroidTvScan);
            consumed = true;
            break;
        }
    }

    return consumed;
}

void wlan_app_scene_network_actions_on_exit(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
}
