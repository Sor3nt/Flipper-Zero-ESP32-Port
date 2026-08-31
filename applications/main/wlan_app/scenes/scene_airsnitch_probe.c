#include "../wlan_app.h"
#include <wlan_hal.h>
#include "../wlan_netscan.h"
#include "../wlan_airsnitch.h"

// AirSnitch-Schritt 2: Erreichbarkeits-Probe vom (Gast-)STA aus.
//   L2: ARP-Sweep des eigenen /24 (gleiches-Subnetz-Fall, wlan_netscan).
//   L3: Gateway-Bounce in gängige Fremd-Subnetze (getrennte-Subnetze-Fall,
//       wlan_airsnitch, eigener xTaskCreate-Worker).
// Beide laufen parallel; wenn beide fertig sind → Ergebnisse mergen → Result.

#define AIRSNITCH_ARP_MAX_TICKS 80 // ~20 s Cap für den L2-Sweep (80 * 250 ms)

static uint16_t s_tick;
static bool s_l2_done;
static bool s_finished;
// popup_set_text speichert nur den Pointer (kopiert nicht) → Buffer muss die
// Tick-Grenze überleben, daher statisch.
static char s_status_text[64];

// L2 (ARP) + L3 (Gateway-Bounce) zu einer Peer-Liste zusammenführen. Eigene IP
// und eigenes (Gast-)Gateway werden ausgeschlossen; Fremd-Subnetz-Gateways, die
// antworten, sind dagegen gültige Leak-Nachweise.
static void airsnitch_merge(WlanApp* app) {
    app->airsnitch_peer_count = 0;

    uint32_t own_ip = wlan_hal_get_own_ip();
    uint32_t gw_ip = wlan_hal_get_gw_ip();

    // L2: gleiches Subnetz (MAC + evtl. Hostname bekannt).
    uint8_t l2 = wlan_netscan_get_host_count();
    for(uint8_t i = 0; i < l2 && app->airsnitch_peer_count < WLAN_AIRSNITCH_MAX_PEERS; ++i) {
        WlanNetscanHost h;
        if(!wlan_netscan_get_host(i, &h)) continue;
        if(h.ip == own_ip || (gw_ip && h.ip == gw_ip)) continue;
        WlanAirsnitchPeer* p = &app->airsnitch_peers[app->airsnitch_peer_count++];
        memset(p, 0, sizeof(*p));
        p->ip = h.ip;
        memcpy(p->mac, h.mac, 6);
        p->same_subnet = true;
        if(h.hostname[0]) {
            strncpy(p->hostname, h.hostname, sizeof(p->hostname) - 1);
        }
    }

    // L3: Fremd-Subnetze (nur IP bekannt).
    uint8_t l3 = wlan_airsnitch_get_count();
    for(uint8_t i = 0; i < l3 && app->airsnitch_peer_count < WLAN_AIRSNITCH_MAX_PEERS; ++i) {
        uint32_t ip = 0;
        if(!wlan_airsnitch_get(i, &ip)) continue;
        if(ip == own_ip || (gw_ip && ip == gw_ip)) continue;
        bool dup = false;
        for(uint8_t k = 0; k < app->airsnitch_peer_count; ++k) {
            if(app->airsnitch_peers[k].ip == ip) {
                dup = true;
                break;
            }
        }
        if(dup) continue;
        WlanAirsnitchPeer* p = &app->airsnitch_peers[app->airsnitch_peer_count++];
        memset(p, 0, sizeof(*p));
        p->ip = ip;
        p->same_subnet = false;
    }
}

void wlan_app_scene_airsnitch_probe_on_enter(void* context) {
    WlanApp* app = context;
    s_tick = 0;
    s_l2_done = false;
    s_finished = false;
    app->airsnitch_peer_count = 0;

    // L2 vorbereiten (liest own ip/netmask), L3-Worker starten.
    wlan_netscan_reset();
    wlan_airsnitch_reset();
    wlan_airsnitch_start();

    popup_reset(app->popup);
    popup_set_header(app->popup, "AirSnitch", 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, "Probing ...", 64, 34, AlignCenter, AlignCenter);
    popup_set_context(app->popup, app);
    popup_disable_timeout(app->popup);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPopup);
}

bool wlan_app_scene_airsnitch_probe_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventAirSnitchProbeDone) {
            scene_manager_next_scene(app->scene_manager, WlanAppSceneAirSnitchResult);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        // Abbruch: Worker stoppen, direkt zurück ins Hauptmenü.
        wlan_airsnitch_stop();
        scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, WlanAppSceneMain);
        consumed = true;
    } else if(event.type == SceneManagerEventTypeTick) {
        if(s_finished) return false;

        if(!s_l2_done) {
            bool arp_done = wlan_netscan_arp_step();
            s_tick++;
            if(arp_done || s_tick >= AIRSNITCH_ARP_MAX_TICKS) s_l2_done = true;
        }
        bool l3_done = wlan_airsnitch_is_done();

        char sub[24];
        wlan_airsnitch_get_status(sub, sizeof(sub));
        snprintf(
            s_status_text, sizeof(s_status_text), "L2 %u  L3 %u%% (%u)\n%s",
            (unsigned)wlan_netscan_get_host_count(),
            (unsigned)wlan_airsnitch_get_progress(),
            (unsigned)wlan_airsnitch_get_count(), sub[0] ? sub : "local /24");
        popup_set_text(app->popup, s_status_text, 64, 34, AlignCenter, AlignCenter);

        if(s_l2_done && l3_done) {
            s_finished = true;
            airsnitch_merge(app);
            view_dispatcher_send_custom_event(
                app->view_dispatcher, WlanAppCustomEventAirSnitchProbeDone);
        }
    }

    return consumed;
}

void wlan_app_scene_airsnitch_probe_on_exit(void* context) {
    WlanApp* app = context;
    wlan_airsnitch_stop(); // idempotent (no-op wenn schon fertig)
    popup_reset(app->popup);
    s_tick = 0;
    s_l2_done = false;
    s_finished = false;
}
