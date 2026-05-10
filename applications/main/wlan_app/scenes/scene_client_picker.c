#include "../wlan_app.h"
#include "../wlan_client_scanner.h"
#include "../wlan_oui.h"
#include <stdlib.h>

static WlanOuiTable* s_oui_table = NULL;

#define CP_ACTION_BASE         2000
#define CP_ACTION_SELECT_ALL   (CP_ACTION_BASE + 0)
#define CP_ACTION_RESCAN       (CP_ACTION_BASE + 1)

// View-Index der Devices: Action(0) + Separator(1) → Devices ab Index 2.
#define CP_DEVICE_VIEW_OFFSET  2

static uint16_t cp_tick;

static int cp_compare_by_rssi(const void* a, const void* b) {
    const WlanDeauthClient* ca = a;
    const WlanDeauthClient* cb = b;
    if(ca->rssi == cb->rssi) return 0;
    return (ca->rssi < cb->rssi) ? 1 : -1; // höhere RSSI zuerst
}

static void cp_sort_by_rssi(WlanApp* app) {
    if(app->deauth_client_count > 1) {
        qsort(app->deauth_clients, app->deauth_client_count,
            sizeof(WlanDeauthClient), cp_compare_by_rssi);
    }
}

static void cp_clear_clients(WlanApp* app) {
    memset(app->deauth_clients, 0, sizeof(app->deauth_clients));
    app->deauth_client_count = 0;
}

// RSSI in dBm → Prozent: -50 dBm und besser = 99%, -100 dBm und schlechter = 10%.
static uint8_t cp_rssi_to_percent(int8_t rssi) {
    if(rssi >= -50) return 99;
    if(rssi <= -100) return 10;
    int p = 10 + (rssi + 100) * 89 / 50;
    if(p > 99) p = 99;
    if(p < 10) p = 10;
    return (uint8_t)p;
}

static void cp_format_rssi(int8_t rssi, char* out, size_t sz) {
    snprintf(out, sz, "%u%%", (unsigned)cp_rssi_to_percent(rssi));
}

static void cp_format_mac(const uint8_t* mac, char* out, size_t sz) {
    snprintf(out, sz, "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void cp_rebuild(WlanApp* app) {
    View* v = app->view_lan;
    wlan_lan_view_clear_menu(v);
    wlan_lan_view_close_menu(v);
    wlan_lan_view_clear(v);

    wlan_lan_view_set_header_title(v, "Clients");
    wlan_lan_view_set_back_label(v, ""); // Hardware-Back genügt → volle Listenhöhe
    wlan_lan_view_set_force_selection_counter(v, true); // immer "<sel>/<total>"

    if(app->deauth_client_count == 0) {
        wlan_lan_view_set_empty_text(v, "Looking for clients...");
        return;
    }
    wlan_lan_view_set_empty_text(v, "");

    wlan_lan_view_add_action_centered(v, "Un/Select all", CP_ACTION_SELECT_ALL);
    wlan_lan_view_add_separator(v);

    for(uint8_t i = 0; i < app->deauth_client_count; ++i) {
        char rssi[8], mac[24], vendor_short[18]; // 17 = MAC-Breite "XX:XX:XX:XX:XX:XX"
        cp_format_rssi(app->deauth_clients[i].rssi, rssi, sizeof(rssi));
        cp_format_mac(app->deauth_clients[i].mac, mac, sizeof(mac));
        const char* vendor = wlan_oui_lookup(s_oui_table, app->deauth_clients[i].mac);
        const char* label;
        if(vendor) {
            snprintf(vendor_short, sizeof(vendor_short), "%s", vendor);
            label = vendor_short;
        } else {
            label = mac;
        }
        const char* val = app->deauth_clients[i].cut ? "*" : "VIP";
        bool is_default = !app->deauth_clients[i].cut;
        wlan_lan_view_add_device_compact(v, rssi, label, val, NULL, is_default, i);
    }

    wlan_lan_view_add_separator(v);
    wlan_lan_view_add_action_centered(v, "Re-Scan", CP_ACTION_RESCAN);

    uint8_t restore = scene_manager_get_scene_state(app->scene_manager, WlanAppSceneClientPicker);
    wlan_lan_view_set_selected(v, restore);
}

static void cp_toggle_all(WlanApp* app) {
    bool any_vip = false;
    for(uint8_t i = 0; i < app->deauth_client_count; ++i) {
        if(!app->deauth_clients[i].cut) {
            any_vip = true;
            break;
        }
    }
    for(uint8_t i = 0; i < app->deauth_client_count; ++i) {
        app->deauth_clients[i].cut = any_vip;
    }
}

// Wählt BSSID-Filter und Channel für den Scanner basierend auf App-State.
// Im Channel-Mode: kein BSSID-Filter (alle Stations auf dem Channel).
// Sonst: BSSID des Targets/Connected-AP.
static void cp_pick_filter(WlanApp* app, const uint8_t** out_bssid, uint8_t* out_channel) {
    if(app->channel_mode_active) {
        *out_bssid = NULL;
        *out_channel = app->channel_action_channel ? app->channel_action_channel : 1;
        return;
    }
    const WlanApRecord* ref = app->target_ap.ssid[0] ? &app->target_ap :
                              (app->connected ? &app->connected_ap : NULL);
    if(ref) {
        *out_bssid = ref->bssid;
        *out_channel = ref->channel ? ref->channel : 1;
    } else {
        *out_bssid = NULL;
        *out_channel = 1;
    }
}

void wlan_app_scene_client_picker_on_enter(void* context) {
    WlanApp* app = context;
    cp_tick = 0;
    // OUI-Tabelle wird deferred im ersten Tick geladen, damit der initiale
    // "Looking for clients..."-Frame nicht durch ~1 s File-I/O blockiert wird.

    // Im Channel-Mode synthetischer Key "_ch_<n>" → Liste pro Channel separat.
    // Sonst SSID des Targets/Connected-AP.
    char current_key[WLAN_APP_SSID_MAX];
    wlan_app_picker_current_key(app, current_key, sizeof(current_key));
    if(strncmp(app->picker_associated_ssid, current_key,
           sizeof(app->picker_associated_ssid)) != 0) {
        cp_clear_clients(app);
        strncpy(app->picker_associated_ssid, current_key,
            sizeof(app->picker_associated_ssid) - 1);
        app->picker_associated_ssid[sizeof(app->picker_associated_ssid) - 1] = '\0';
    }

    cp_sort_by_rssi(app);
    cp_rebuild(app);

    const uint8_t* bssid;
    uint8_t channel;
    cp_pick_filter(app, &bssid, &channel);
    wlan_client_scanner_start(bssid, channel);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewLan);
}

bool wlan_app_scene_client_picker_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom &&
       event.event == WlanAppCustomEventLanItemOk) {
        uint8_t sel = wlan_lan_view_get_selected(app->view_lan);
        WlanLanItem it = wlan_lan_view_get_item(app->view_lan, sel);
        scene_manager_set_scene_state(app->scene_manager, WlanAppSceneClientPicker, sel);

        if(it.kind == WlanLanItemKindDevice) {
            uint16_t ci = it.user_id;
            if(ci < app->deauth_client_count) {
                app->deauth_clients[ci].cut = !app->deauth_clients[ci].cut;
                wlan_lan_view_set_device_value_by_user_id(
                    app->view_lan, ci,
                    app->deauth_clients[ci].cut ? "*" : "VIP",
                    NULL,
                    !app->deauth_clients[ci].cut);
            }
            consumed = true;
        } else if(it.user_id == CP_ACTION_SELECT_ALL) {
            cp_toggle_all(app);
            cp_rebuild(app);
            consumed = true;
        } else if(it.user_id == CP_ACTION_RESCAN) {
            cp_clear_clients(app);
            scene_manager_set_scene_state(app->scene_manager, WlanAppSceneClientPicker, 0);
            // Scanner mit aktuellem Filter neu starten — leert auch den Ring.
            wlan_client_scanner_stop();
            const uint8_t* bssid;
            uint8_t channel;
            cp_pick_filter(app, &bssid, &channel);
            wlan_client_scanner_start(bssid, channel);
            cp_rebuild(app);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        cp_tick++;
        // Erster Tick: View ist gezeichnet — jetzt Vendor-Tabelle laden,
        // ohne den initialen "Looking for clients..."-Frame zu blockieren.
        if(cp_tick == 1 && !s_oui_table) {
            s_oui_table = wlan_oui_load();
            if(app->deauth_client_count > 0) cp_rebuild(app);
        }
        // Drain bei jedem Tick (~250 ms), Rebuild gedrosselt auf ~1 s und
        // nur wenn sich tatsächlich etwas verändert hat.
        bool changed = wlan_client_scanner_drain(app);
        if(changed && (cp_tick % 4 == 0)) {
            // Aktuelle Selection per MAC merken, damit der Cursor das
            // Einfügen/Re-Sort der Liste übersteht.
            uint8_t cur_view_sel = wlan_lan_view_get_selected(app->view_lan);
            WlanLanItem cur_item = wlan_lan_view_get_item(app->view_lan, cur_view_sel);
            bool track_device = (cur_item.kind == WlanLanItemKindDevice);
            uint8_t track_mac[6] = {0};
            if(track_device && cur_item.user_id < app->deauth_client_count) {
                memcpy(track_mac, app->deauth_clients[cur_item.user_id].mac, 6);
            }

            cp_sort_by_rssi(app);

            uint8_t restore = cur_view_sel;
            if(track_device) {
                for(uint8_t i = 0; i < app->deauth_client_count; ++i) {
                    if(memcmp(app->deauth_clients[i].mac, track_mac, 6) == 0) {
                        restore = (uint8_t)(CP_DEVICE_VIEW_OFFSET + i);
                        break;
                    }
                }
            }
            scene_manager_set_scene_state(
                app->scene_manager, WlanAppSceneClientPicker, restore);

            cp_rebuild(app);
        }
    }

    return consumed;
}

void wlan_app_scene_client_picker_on_exit(void* context) {
    WlanApp* app = context;
    wlan_client_scanner_stop();
    if(s_oui_table) {
        wlan_oui_free(s_oui_table);
        s_oui_table = NULL;
    }
    // Selektionen bleiben erhalten — sie werden erst beim SSID-Wechsel
    // (in on_enter) verworfen oder via "Re-Scan" zurückgesetzt.
    wlan_lan_view_set_header_title(app->view_lan, "LAN");
    wlan_lan_view_set_back_label(app->view_lan, "");
    wlan_lan_view_set_empty_text(app->view_lan, "");
    wlan_lan_view_set_force_selection_counter(app->view_lan, false);
    wlan_lan_view_clear(app->view_lan);
    cp_tick = 0;
}
