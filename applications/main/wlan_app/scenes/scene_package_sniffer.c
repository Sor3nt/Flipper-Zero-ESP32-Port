#include "../wlan_app.h"
#include "../wlan_hal.h"
#include <esp_wifi.h>
#include <string.h>

#define SNIFFER_TICK_MS 250
#define SNIFFER_TICKS_PER_SEC (1000 / SNIFFER_TICK_MS)

#define SNIFFER_CHANNEL_MIN 1
#define SNIFFER_CHANNEL_MAX 13

enum SnifferEvent {
    SnifferEventToggle = 1,
    SnifferEventTargets,
    SnifferEventChannelUp,
    SnifferEventChannelDown,
};

// Filter-Modus für den Promisc-Callback.
typedef enum {
    SnifFilterAll,        // Channel-Mode: alle Frames mitzählen
    SnifFilterBssid,      // SSID-Mode ohne Selection: Frames mit BSSID-Match
    SnifFilterUnicastSet, // SSID-Mode mit Selection: Frames mit STA in Liste
} SnifFilterMode;

static bool s_sniff_running;
static uint16_t s_sniff_tick;
static volatile uint32_t s_received;
static uint32_t s_elapsed_sec;

// Filter-Konfiguration (vom Scene-Code befüllt, vom Callback gelesen).
static volatile SnifFilterMode s_filter_mode = SnifFilterAll;
static uint8_t s_filter_bssid[6];
static uint8_t s_filter_macs[WLAN_APP_MAX_DEAUTH_CLIENTS][6];
static volatile uint8_t s_filter_mac_count = 0;

static bool sniff_match_unicast(const uint8_t* mac) {
    uint8_t n = s_filter_mac_count;
    for(uint8_t i = 0; i < n; i++) {
        if(memcmp(s_filter_macs[i], mac, 6) == 0) return true;
    }
    return false;
}

static void sniff_promisc_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    const wifi_promiscuous_pkt_t* pkt = buf;
    int len = pkt->rx_ctrl.sig_len;
    if(len < 10) return;

    // Ctrl-Frames (ACK/RTS/CTS …) konsistent in allen Modi ausschließen — sonst
    // zählt der All-Mode auch eigene ACKs mit, was die Größenordnung verzerrt.
    if(type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT) return;

    SnifFilterMode mode = s_filter_mode;
    if(mode == SnifFilterAll) {
        s_received++;
        return;
    }

    // Mgmt/Data: Header-Filter braucht volle 24 Byte (addr1..addr3).
    if(len < 24) return;

    const uint8_t* p = pkt->payload;
    uint16_t fc = p[0] | (p[1] << 8);
    uint8_t to_ds = (fc & 0x0100) >> 8;
    uint8_t from_ds = (fc & 0x0200) >> 9;

    const uint8_t* addr1 = &p[4];
    const uint8_t* addr2 = &p[10];
    const uint8_t* addr3 = &p[16];

    const uint8_t* bssid;
    const uint8_t* sta_a; // mögliche STA-Adresse 1
    const uint8_t* sta_b; // mögliche STA-Adresse 2
    if(to_ds && !from_ds) {
        bssid = addr1; sta_a = addr2; sta_b = NULL;
    } else if(!to_ds && from_ds) {
        bssid = addr2; sta_a = addr1; sta_b = NULL;
    } else if(!to_ds && !from_ds) {
        bssid = addr3; sta_a = addr1; sta_b = addr2;
    } else {
        return; // WDS — ignorieren
    }

    if(mode == SnifFilterBssid) {
        if(memcmp(bssid, s_filter_bssid, 6) == 0) s_received++;
        return;
    }
    if(mode == SnifFilterUnicastSet) {
        if(memcmp(bssid, s_filter_bssid, 6) != 0) return;
        if(sniff_match_unicast(sta_a)) {
            s_received++;
            return;
        }
        if(sta_b && sniff_match_unicast(sta_b)) s_received++;
    }
}

static void sniff_apply_filter(WlanApp* app) {
    if(app->channel_mode_active) {
        s_filter_mode = SnifFilterAll;
        s_filter_mac_count = 0;
        memset(s_filter_bssid, 0, 6);
        return;
    }
    const WlanApRecord* src = NULL;
    if(app->target_ap.ssid[0]) src = &app->target_ap;
    else if(app->connected) src = &app->connected_ap;
    if(src) {
        memcpy(s_filter_bssid, src->bssid, 6);
    } else {
        memset(s_filter_bssid, 0, 6);
    }

    // Picker-Selektionen nur verwenden wenn sie zum aktuellen Target passen —
    // sonst sind sie stale (alter AP) und würden den Filter ins Leere laufen lassen.
    uint8_t n = 0;
    if(wlan_app_picker_matches_current(app)) {
        for(uint8_t i = 0; i < app->deauth_client_count &&
                           n < WLAN_APP_MAX_DEAUTH_CLIENTS; ++i) {
            if(app->deauth_clients[i].cut) {
                memcpy(s_filter_macs[n], app->deauth_clients[i].mac, 6);
                n++;
            }
        }
    }
    s_filter_mac_count = n;
    s_filter_mode = (n > 0) ? SnifFilterUnicastSet : SnifFilterBssid;
}

static uint8_t sniff_pick_channel(WlanApp* app) {
    if(app->channel_mode_active) {
        if(app->channel_action_channel < SNIFFER_CHANNEL_MIN ||
           app->channel_action_channel > SNIFFER_CHANNEL_MAX) {
            app->channel_action_channel = SNIFFER_CHANNEL_MIN;
        }
        return app->channel_action_channel;
    }
    const WlanApRecord* src = NULL;
    if(app->target_ap.ssid[0]) src = &app->target_ap;
    else if(app->connected) src = &app->connected_ap;
    return (src && src->channel) ? src->channel : 1;
}

static bool sniff_has_valid_target(const WlanApp* app) {
    if(app->channel_mode_active) return true;
    return app->target_ap.ssid[0] || app->connected;
}

static bool sniff_start(WlanApp* app) {
    if(!sniff_has_valid_target(app)) return false;
    if(!wlan_hal_is_started()) wlan_hal_start();
    if(wlan_hal_is_connected()) wlan_hal_disconnect();
    sniff_apply_filter(app);
    wlan_hal_set_channel(sniff_pick_channel(app));
    wlan_hal_set_promiscuous(true, sniff_promisc_cb);
    return true;
}

static void sniff_stop(void) {
    wlan_hal_set_promiscuous(false, NULL);
}

static void sniffer_view_action_cb(WlanSnifferViewAction action, void* ctx) {
    WlanApp* app = ctx;
    uint32_t event = 0;
    switch(action) {
    case WlanSnifferViewActionToggle:
        event = SnifferEventToggle;
        break;
    case WlanSnifferViewActionTargets:
        event = SnifferEventTargets;
        break;
    case WlanSnifferViewActionChannelUp:
        event = SnifferEventChannelUp;
        break;
    case WlanSnifferViewActionChannelDown:
        event = SnifferEventChannelDown;
        break;
    }
    view_dispatcher_send_custom_event(app->view_dispatcher, event);
}

void wlan_app_scene_package_sniffer_on_enter(void* context) {
    WlanApp* app = context;
    s_sniff_running = false;
    s_sniff_tick = 0;
    s_received = 0;
    s_elapsed_sec = 0;

    wlan_sniffer_view_reset_counters(app->sniffer_view_obj);
    wlan_sniffer_view_set_running(app->sniffer_view_obj, false);
    wlan_sniffer_view_set_channel_mode(app->sniffer_view_obj, app->channel_mode_active);

    if(app->channel_mode_active) {
        if(app->channel_action_channel < SNIFFER_CHANNEL_MIN ||
           app->channel_action_channel > SNIFFER_CHANNEL_MAX) {
            app->channel_action_channel = SNIFFER_CHANNEL_MIN;
        }
        wlan_sniffer_view_set_title(app->sniffer_view_obj, "Channel sniffer");
        wlan_sniffer_view_set_channel(app->sniffer_view_obj, app->channel_action_channel);
        wlan_sniffer_view_set_target_count(app->sniffer_view_obj, 0);
    } else {
        const char* title;
        if(!sniff_has_valid_target(app)) {
            title = "No target";
        } else if(wlan_app_picker_has_selection(app)) {
            title = "Target sniffer";
        } else {
            title = "Network sniffer";
        }
        wlan_sniffer_view_set_title(app->sniffer_view_obj, title);
        wlan_sniffer_view_set_target_count(
            app->sniffer_view_obj, wlan_app_picker_selection_count(app));
    }

    wlan_sniffer_view_set_action_callback(
        app->sniffer_view_obj, sniffer_view_action_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSniffer);
}

bool wlan_app_scene_package_sniffer_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SnifferEventToggle) {
            if(!s_sniff_running) {
                // Beim Start frische Messung — Counter hängen bleibt sonst
                // beim alten Wert, was nach Channel-Wechsel verwirrt.
                s_received = 0;
                s_elapsed_sec = 0;
                s_sniff_tick = 0;
                wlan_sniffer_view_reset_counters(app->sniffer_view_obj);
                if(sniff_start(app)) {
                    s_sniff_running = true;
                    wlan_sniffer_view_set_running(app->sniffer_view_obj, true);
                }
                // sniff_start kann scheitern (z.B. SSID-Mode ohne Target) —
                // running bleibt false, View ebenfalls.
            } else {
                s_sniff_running = false;
                wlan_sniffer_view_set_running(app->sniffer_view_obj, false);
                sniff_stop();
            }
            consumed = true;
        } else if(event.event == SnifferEventTargets) {
            // Sniffing während Target-Auswahl pausieren.
            s_sniff_running = false;
            wlan_sniffer_view_set_running(app->sniffer_view_obj, false);
            sniff_stop();
            scene_manager_next_scene(app->scene_manager, WlanAppSceneClientPicker);
            consumed = true;
        } else if(event.event == SnifferEventChannelDown) {
            if(app->channel_action_channel < SNIFFER_CHANNEL_MAX) {
                app->channel_action_channel++;
                wlan_sniffer_view_set_channel(
                    app->sniffer_view_obj, app->channel_action_channel);
                if(s_sniff_running) wlan_hal_set_channel(app->channel_action_channel);
            }
            consumed = true;
        } else if(event.event == SnifferEventChannelUp) {
            if(app->channel_action_channel > SNIFFER_CHANNEL_MIN) {
                app->channel_action_channel--;
                wlan_sniffer_view_set_channel(
                    app->sniffer_view_obj, app->channel_action_channel);
                if(s_sniff_running) wlan_hal_set_channel(app->channel_action_channel);
            }
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick && s_sniff_running) {
        s_sniff_tick++;
        if(s_sniff_tick % SNIFFER_TICKS_PER_SEC == 0) s_elapsed_sec++;
        wlan_sniffer_view_set_received(app->sniffer_view_obj, s_received);
        wlan_sniffer_view_set_elapsed(app->sniffer_view_obj, s_elapsed_sec);
    }

    return consumed;
}

void wlan_app_scene_package_sniffer_on_exit(void* context) {
    WlanApp* app = context;
    sniff_stop();
    wlan_sniffer_view_set_action_callback(app->sniffer_view_obj, NULL, NULL);
    wlan_sniffer_view_set_running(app->sniffer_view_obj, false);
    wlan_sniffer_view_set_channel_mode(app->sniffer_view_obj, false);
    s_sniff_running = false;
    s_sniff_tick = 0;
    s_received = 0;
    s_filter_mac_count = 0;
}
