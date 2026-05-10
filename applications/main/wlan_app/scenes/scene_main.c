#include "../wlan_app.h"

enum MainIndex {
    MainIndexSelectWifi,
    MainIndexAttack,
    MainIndexSwitchWifi,
    MainIndexDisconnect,
    // Channel-Aktionen (immer sichtbar, unter dem Separator).
    MainIndexChannelHandshake = 10,
    MainIndexChannelDeauth = 11,
    MainIndexChannelSniffer = 12,
    MainIndexChannelEvilPortal = 13,
};

static void wlan_app_scene_main_submenu_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wlan_app_scene_main_on_enter(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
    submenu_set_header_centered(app->submenu, "WiFi");

    // Channel-Aktionen sind immer sichtbar; Verbindungs-Aktionen sind state-abhängig.
    app->channel_mode_active = false;

    if(!app->connected && !app->target_selected) {
        submenu_add_item_centered(
            app->submenu, "Select Wifi", MainIndexSelectWifi, wlan_app_scene_main_submenu_cb, app);
    } else if(app->connected) {
        char label[48];
        snprintf(label, sizeof(label), "%s Attack", app->connected_ap.ssid);
        submenu_add_item(
            app->submenu, label, MainIndexAttack, wlan_app_scene_main_submenu_cb, app);
        submenu_add_item(
            app->submenu, "Switch Wifi", MainIndexSwitchWifi, wlan_app_scene_main_submenu_cb, app);
        submenu_add_item(
            app->submenu, "Disconnect", MainIndexDisconnect, wlan_app_scene_main_submenu_cb, app);
    } else {
        char label[48];
        snprintf(label, sizeof(label), "%s Attack", app->target_ap.ssid);
        submenu_add_item(
            app->submenu, label, MainIndexAttack, wlan_app_scene_main_submenu_cb, app);
        submenu_add_item(
            app->submenu, "Switch Wifi", MainIndexSwitchWifi, wlan_app_scene_main_submenu_cb, app);
    }

    submenu_add_separator(app->submenu);

    submenu_add_item(
        app->submenu, "Capture Handshake", MainIndexChannelHandshake,
        wlan_app_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Deauth", MainIndexChannelDeauth,
        wlan_app_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Sniffer", MainIndexChannelSniffer,
        wlan_app_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Evil Portal", MainIndexChannelEvilPortal,
        wlan_app_scene_main_submenu_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSubmenu);
}

bool wlan_app_scene_main_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case MainIndexSelectWifi:
        case MainIndexSwitchWifi:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneConnect);
            consumed = true;
            break;
        case MainIndexAttack:
            if(app->connected) {
                // Erster Aufruf nach Connect/Disconnect: ARP-Scan; danach
                // direkt zur LAN-Liste, bis Re-Scan getriggert wird.
                if(app->lan_scan_complete) {
                    scene_manager_next_scene(app->scene_manager, WlanAppSceneLan);
                } else {
                    scene_manager_next_scene(app->scene_manager, WlanAppSceneNetworkScanning);
                }
            } else {
                scene_manager_next_scene(app->scene_manager, WlanAppSceneNetworkActions);
            }
            consumed = true;
            break;
        case MainIndexDisconnect:
            // Session 1: state-only toggle
            app->connected = false;
            app->target_selected = false;
            app->lan_scan_complete = false;
            memset(&app->connected_ap, 0, sizeof(app->connected_ap));
            wlan_app_scene_main_on_exit(app);
            wlan_app_scene_main_on_enter(app);
            consumed = true;
            break;
        case MainIndexChannelHandshake:
            app->channel_mode_active = true;
            if(app->channel_action_channel == 0) app->channel_action_channel = 1;
            scene_manager_next_scene(app->scene_manager, WlanAppSceneHandshake);
            consumed = true;
            break;
        case MainIndexChannelDeauth:
            app->channel_mode_active = true;
            if(app->channel_action_channel == 0) app->channel_action_channel = 1;
            scene_manager_next_scene(app->scene_manager, WlanAppSceneNetworkDeauth);
            consumed = true;
            break;
        case MainIndexChannelSniffer:
            app->channel_mode_active = true;
            if(app->channel_action_channel == 0) app->channel_action_channel = 1;
            scene_manager_next_scene(app->scene_manager, WlanAppScenePackageSniffer);
            consumed = true;
            break;
        case MainIndexChannelEvilPortal:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneEvilPortalMenu);
            consumed = true;
            break;
        }
    }

    return consumed;
}

void wlan_app_scene_main_on_exit(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
}
