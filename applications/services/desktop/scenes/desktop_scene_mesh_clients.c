/**
 * Mesh-Clients-Scene (Master): zeigt gepairte + discovered Clients und treibt
 * Discovery/Pair/Remove. Der Mesh-Service läuft nur während diese Scene aktiv
 * ist — beim Verlassen wird er gestoppt (und ein eventuell aktiver Client-
 * Service-Modus ist hier nicht aktiv, weil mesh_mode==Master).
 *
 * Discovery-Timer: alle 30 s ein DiscoverReq via Broadcast. Erste Discovery
 * sofort beim Enter.
 *
 * Pair-Flow:
 *   - User OK auf ungepairtem Eintrag → mesh_send_pair_request, Footer auf
 *     "Wait for Accept", Pair-Timeout-Timer (15 s) startet.
 *   - PairResponse(accepted=true)  → in clients.txt persistieren, Liste refreshen,
 *     Footer zurück, Timer stoppen.
 *   - PairResponse(accepted=false) → Eintrag bleibt ungepairt; Footer zurück.
 *   - Timeout                       → still verwerfen, Footer zurück.
 */

#include <furi.h>
#include <gui/scene_manager.h>

#include "../desktop_i.h"
#include "../views/desktop_view_mesh_clients.h"
#include "../helpers/mesh_config.h"
#include "../helpers/mesh_service.h"
#include "../helpers/mesh_capture.h"
#include "desktop_scene.h"

#define TAG "DesktopMeshClients"

#define PAIR_TIMEOUT_MS 15000
#define SWEEP_PERIOD_MS 600 /* pro Kanal beim Discovery-Sweep */

/* Live-Status eines Clients — NICHT persistent. Quelle ist immer der Buddy
 * (FeatureList.running_mask). Bis er geantwortet hat: reported=false → "wait...".
 * running_mask==0 → "Idle", sonst Name der laufenden Action. channel = Kanal, auf
 * dem der Buddy zuletzt geantwortet hat (für gezieltes Tunen). */
typedef struct {
    uint8_t mac[MESH_MAC_LEN];
    bool reported;
    char label[20];
    uint8_t channel; /* 0 = unbekannt */
    /* Zuletzt vom Buddy gemeldete Feature-Liste — für sofortige Anzeige in der
     * Action-Scene (kein zweites Warten). */
    MeshFeature features[MESH_FEATURES_MAX];
    uint8_t feature_count;
    uint32_t running_mask;
} ClientStatus;

/* Scene-State (single-instance Scene → file-static OK). */
static struct {
    /* Vereinigte Liste: erst gepairte, dann session-discovered. Wird beim
     * Refresh aus mesh_config_load_clients + s_discovered neu aufgebaut. */
    MeshPeer all[MESH_CLIENTS_MAX];
    bool paired[MESH_CLIENTS_MAX];
    size_t all_count;

    /* In dieser Scene-Session per Discovery entdeckte Peers, die noch nicht
     * gepairt sind. Beim Verlassen weggeworfen. */
    MeshPeer discovered[MESH_CLIENTS_MAX];
    size_t discovered_count;

    /* Transienter Live-Status-Cache (nur diese Scene-Session). */
    ClientStatus status[MESH_CLIENTS_MAX];
    size_t status_count;

    FuriTimer* sweep_timer; /* rotiert die Kanäle: Discovery + Status-Query je Kanal */
    FuriTimer* pair_timer;
    uint8_t sweep_ch; /* aktueller Sweep-Kanal (1..13) */
    uint8_t sweep_next_non1; /* nächster Nicht-ch1-Kanal (2..13) für den Sweep */

    /* MAC des aktuellen Pair-Requests (für Timeout-Recovery). */
    uint8_t pair_target[MESH_MAC_LEN];
    bool pair_in_progress;
} s_state;

/* Status-Cache-Eintrag suchen / anlegen (default reported=false → "wait..."). */
static ClientStatus* status_find(const uint8_t mac[MESH_MAC_LEN]) {
    for(size_t i = 0; i < s_state.status_count; ++i) {
        if(memcmp(s_state.status[i].mac, mac, MESH_MAC_LEN) == 0) return &s_state.status[i];
    }
    return NULL;
}

static ClientStatus* status_ensure(const uint8_t mac[MESH_MAC_LEN]) {
    ClientStatus* st = status_find(mac);
    if(st) return st;
    if(s_state.status_count >= MESH_CLIENTS_MAX) return NULL;
    st = &s_state.status[s_state.status_count++];
    memcpy(st->mac, mac, MESH_MAC_LEN);
    st->reported = false;
    st->label[0] = '\0';
    return st;
}

static void desktop_scene_mesh_clients_refresh_view(Desktop* desktop) {
    /* Liste neu zusammenbauen: gepairte (aus clients.txt) + discovered nicht-
     * gepairte. Bei Konflikt MAC: paired hat Vorrang. */
    MeshPeer paired[MESH_CLIENTS_MAX];
    size_t paired_count = 0;
    mesh_config_load_clients(paired, &paired_count);

    size_t n = 0;
    for(size_t i = 0; i < paired_count && n < MESH_CLIENTS_MAX; ++i) {
        s_state.all[n] = paired[i];
        s_state.paired[n] = true;
        n++;
    }
    for(size_t i = 0; i < s_state.discovered_count && n < MESH_CLIENTS_MAX; ++i) {
        bool dup = false;
        for(size_t j = 0; j < paired_count; ++j) {
            if(memcmp(s_state.discovered[i].mac, paired[j].mac, MESH_MAC_LEN) == 0) {
                dup = true;
                break;
            }
        }
        if(dup) continue;
        s_state.all[n] = s_state.discovered[i];
        s_state.paired[n] = false;
        n++;
    }
    s_state.all_count = n;

    /* Status-Label je gepairtem Client kommt live vom Buddy (kein Caching am
     * Master): bis zur Antwort "wait...", sonst "Idle" / Action-Name. */
    const char* status[MESH_CLIENTS_MAX];
    for(size_t i = 0; i < n; ++i) {
        status[i] = NULL;
        if(s_state.paired[i]) {
            ClientStatus* st = status_ensure(s_state.all[i].mac);
            status[i] = (st && st->reported) ? st->label : "wait...";
        }
    }

    desktop_mesh_clients_set_peers(
        desktop->mesh_clients_view, s_state.all, s_state.paired, status, n);
}

/* Status-Query an alle gepairten Clients (Buddy meldet seine running_mask). */
static void query_all_paired(void) {
    for(size_t i = 0; i < s_state.all_count; ++i) {
        if(s_state.paired[i]) mesh_send_feature_query(s_state.all[i].mac);
    }
}

/* Client-Name aus der Liste per MAC (für pcap-Dateinamen). */
static const char* client_name_by_mac(const uint8_t mac[MESH_MAC_LEN]) {
    for(size_t i = 0; i < s_state.all_count; ++i) {
        if(memcmp(s_state.all[i].mac, mac, MESH_MAC_LEN) == 0) return s_state.all[i].name;
    }
    return "buddy";
}

/* FeatureList → Label aus running_mask + Feature-Namen ableiten. Läuft ein
 * Capture und wir sammeln noch nicht (z.B. nach Master-Reboot), hängen wir uns
 * an, um die Ergebnisse einzusammeln. */
static void status_apply_featurelist(const MeshEventData* ev) {
    ClientStatus* st = status_ensure(ev->mac);
    if(!st) return;
    st->reported = true;
    if(ev->rx_channel) st->channel = ev->rx_channel; /* wo der Buddy gerade ist */

    /* Feature-Liste cachen, damit die Action-Scene sie sofort anzeigen kann. */
    st->feature_count = ev->feature_count > MESH_FEATURES_MAX ? MESH_FEATURES_MAX :
                                                                ev->feature_count;
    memcpy(st->features, ev->features, st->feature_count * sizeof(MeshFeature));
    st->running_mask = ev->running_mask;

    if(ev->running_mask == 0) {
        strlcpy(st->label, "Idle", sizeof(st->label));
        return;
    }
    /* Laufendes Feature wählen: Capture bevorzugt, sonst erstes laufendes. */
    const char* chosen = NULL;
    int cap_feat = -1;
    for(uint8_t i = 0; i < ev->feature_count; ++i) {
        if(!(ev->running_mask & (1u << ev->features[i].id))) continue;
        if(strncmp(ev->features[i].name, "Capture", 7) == 0) {
            chosen = ev->features[i].name;
            cap_feat = ev->features[i].id;
            break;
        }
        if(!chosen) chosen = ev->features[i].name;
    }
    strlcpy(st->label, chosen ? chosen : "active", sizeof(st->label));

    /* Auto-Resume der pcap-Sammlung für einen bereits laufenden Capture — auf dem
     * Kanal, auf dem der Buddy gerade gefunden wurde. */
    if(cap_feat >= 0 && !mesh_capture_is_active()) {
        mesh_capture_attach(
            ev->mac, client_name_by_mac(ev->mac), (uint8_t)cap_feat, st->channel);
    }
}

static void desktop_scene_mesh_clients_add_discovered(const MeshPeer* p) {
    /* Dedup gegen schon-discovered und gegen paired. */
    for(size_t i = 0; i < s_state.discovered_count; ++i) {
        if(memcmp(s_state.discovered[i].mac, p->mac, MESH_MAC_LEN) == 0) {
            /* Name evtl. aktualisieren. */
            s_state.discovered[i] = *p;
            return;
        }
    }
    if(s_state.discovered_count >= MESH_CLIENTS_MAX) return;
    s_state.discovered[s_state.discovered_count++] = *p;
}

/* Discovery-Sweep: rotiert die WiFi-Kanäle (der Buddy bleibt auf seinem). Pro
 * Kanal: Broadcast-Discover (ungepairte finden) + Status-Query an alle gepairten
 * (der Buddy auf diesem Kanal antwortet, RX-Kanal verrät seinen Kanal). */
static void sweep_tick(void* ctx) {
    (void)ctx;
    if(s_state.pair_in_progress) return; /* während Pairing nicht weghoppen */
    /* Beim Einsammeln eines Captures bleibt der Master auf dessen Kanal (NICHT
     * rotieren), fragt den Buddy dort aber weiter nach Status (sonst "wait..."). */
    if(mesh_capture_is_active()) {
        query_all_paired();
        return;
    }
    /* Bias auf ch1 (idle-Buddys sind dort): jeder zweite Tick ch1, dazwischen
     * der nächste Nicht-ch1-Kanal (für Recovery eines capturenden Buddys). */
    if(s_state.sweep_ch != 1) {
        s_state.sweep_ch = 1;
    } else {
        s_state.sweep_ch = s_state.sweep_next_non1 ? s_state.sweep_next_non1 : 2;
        s_state.sweep_next_non1 = (s_state.sweep_ch >= 13) ? 2 : (uint8_t)(s_state.sweep_ch + 1);
    }
    mesh_service_set_channel(s_state.sweep_ch);
    mesh_send_discover();
    query_all_paired();
}

static void pair_timeout(void* ctx) {
    Desktop* desktop = ctx;
    if(!s_state.pair_in_progress) return;
    FURI_LOG_W(TAG, "pair timeout");
    s_state.pair_in_progress = false;
    desktop_mesh_clients_set_pairing(desktop->mesh_clients_view, false);
}

static void desktop_scene_mesh_clients_view_cb(DesktopEvent event, void* ctx) {
    Desktop* desktop = ctx;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, event);
}

/* ─────── on_enter / on_event / on_exit ─────── */

void desktop_scene_mesh_clients_on_enter(void* context) {
    Desktop* desktop = context;

    memset(&s_state, 0, sizeof(s_state));
    /* Normales Enter (auch Rückkehr von der Action-Scene): Service soll beim
     * nächsten normalen Verlassen (zurück ins Lock-Menü) gestoppt werden. */
    desktop->mesh_keep_service = false;

    desktop_mesh_clients_set_callback(
        desktop->mesh_clients_view, desktop_scene_mesh_clients_view_cb, desktop);
    desktop_mesh_clients_set_pairing(desktop->mesh_clients_view, false);

    /* Mesh-Service als Master starten. Wenn schon (von einem früheren Re-Enter
     * der Scene) aktiv, ist der Call ein no-op. */
    if(!mesh_service_start(MeshRoleMaster, desktop_mesh_event_cb, desktop)) {
        FURI_LOG_E(TAG, "mesh_service_start failed");
    }

    desktop_scene_mesh_clients_refresh_view(desktop);

    /* Sweep-Timer (rotiert Kanäle: Discovery + Status je Kanal) + Pair-Timeout. */
    s_state.sweep_ch = 0; /* erster Tick → ch1 */
    s_state.sweep_timer = furi_timer_alloc(sweep_tick, FuriTimerTypePeriodic, desktop);
    s_state.pair_timer = furi_timer_alloc(pair_timeout, FuriTimerTypeOnce, desktop);
    furi_timer_start(s_state.sweep_timer, furi_ms_to_ticks(SWEEP_PERIOD_MS));
    sweep_tick(desktop); /* sofort erster Sweep-Schritt (Anzeige bis dahin: "wait...") */

    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdMeshClients);
}

bool desktop_scene_mesh_clients_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = context;
    bool consumed = false;

    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(event.event) {
    case DesktopMeshClientsEventBack:
        scene_manager_search_and_switch_to_previous_scene(
            desktop->scene_manager, DesktopSceneLockMenu);
        consumed = true;
        break;

    case DesktopMeshClientsEventPair: {
        int idx = desktop_mesh_clients_get_selected_idx(desktop->mesh_clients_view);
        if(idx < 0 || (size_t)idx >= s_state.all_count) break;
        if(s_state.paired[idx]) break; /* schon gepairt */
        if(s_state.pair_in_progress) break;

        memcpy(s_state.pair_target, s_state.all[idx].mac, MESH_MAC_LEN);
        /* Auf den Kanal des Buddys tunen (frischer Buddy = ch1) und während des
         * Pairings nicht weghoppen (pair_in_progress pausiert den Sweep). */
        ClientStatus* pst = status_find(s_state.pair_target);
        mesh_service_set_channel(pst && pst->channel ? pst->channel : 1);
        if(!mesh_send_pair_request(s_state.pair_target)) {
            FURI_LOG_W(TAG, "pair_request enqueue failed");
            break;
        }
        s_state.pair_in_progress = true;
        desktop_mesh_clients_set_pairing(desktop->mesh_clients_view, true);
        furi_timer_start(s_state.pair_timer, furi_ms_to_ticks(PAIR_TIMEOUT_MS));
        consumed = true;
        break;
    }

    case DesktopMeshClientsEventOpenAction: {
        int idx = desktop_mesh_clients_get_selected_idx(desktop->mesh_clients_view);
        if(idx < 0 || (size_t)idx >= s_state.all_count) break;
        if(!s_state.paired[idx]) break; /* nur gepairte Clients haben Actions */

        desktop->mesh_action_client = s_state.all[idx];
        desktop->mesh_keep_service = true; /* Service über den Scene-Wechsel halten */

        /* Bereits bekannte Feature-Liste + Kanal mitgeben; auf den Kanal tunen,
         * damit die Abfragen der Action-Scene den Buddy erreichen. */
        ClientStatus* st = status_find(s_state.all[idx].mac);
        uint8_t ch = (st && st->channel) ? st->channel : 1;
        desktop->mesh_action_channel = ch;
        mesh_service_set_channel(ch);
        if(st && st->reported && st->feature_count > 0) {
            desktop->mesh_action_feature_count = st->feature_count;
            memcpy(
                desktop->mesh_action_features,
                st->features,
                st->feature_count * sizeof(MeshFeature));
            desktop->mesh_action_running_mask = st->running_mask;
        } else {
            desktop->mesh_action_feature_count = 0;
        }

        scene_manager_next_scene(desktop->scene_manager, DesktopSceneMeshAction);
        consumed = true;
        break;
    }

    case DesktopMeshClientsEventRemove: {
        int idx = desktop_mesh_clients_get_selected_idx(desktop->mesh_clients_view);
        if(idx < 0 || (size_t)idx >= s_state.all_count) break;
        if(!s_state.paired[idx]) break;

        /* Laufende Capture-Session dieses Clients beenden. */
        uint8_t cap_mac[MESH_MAC_LEN];
        if(mesh_capture_is_active() && mesh_capture_get_mac(cap_mac) &&
           memcmp(cap_mac, s_state.all[idx].mac, MESH_MAC_LEN) == 0) {
            mesh_capture_finish();
        }

        mesh_send_disconnect(s_state.all[idx].mac);
        mesh_config_remove_client(s_state.all[idx].mac);
        desktop_scene_mesh_clients_refresh_view(desktop);
        consumed = true;
        break;
    }

    case DesktopMeshEventMasterDiscoverRsp: {
        /* Daten kommen über desktop->mesh_pending (vom Mesh-Service vor dem
         * Custom-Event befüllt). */
        MeshPeer p = {0};
        memcpy(p.mac, desktop->mesh_pending.mac, MESH_MAC_LEN);
        memcpy(p.name, desktop->mesh_pending.name, sizeof(p.name));
        desktop_scene_mesh_clients_add_discovered(&p);
        /* Kanal merken (für gezieltes Tunen bei Pair/Action). */
        ClientStatus* dst = status_ensure(desktop->mesh_pending.mac);
        if(dst && desktop->mesh_pending.rx_channel) dst->channel = desktop->mesh_pending.rx_channel;
        desktop_scene_mesh_clients_refresh_view(desktop);
        consumed = true;
        break;
    }

    case DesktopMeshEventMasterPairRsp: {
        /* Nur reagieren wenn die Antwort zum aktuell laufenden Pair-Request
         * passt — sonst ist es eine Spät-Antwort eines abgebrochenen Pairs. */
        if(!s_state.pair_in_progress) {
            consumed = true;
            break;
        }
        if(memcmp(desktop->mesh_pending.mac, s_state.pair_target, MESH_MAC_LEN) != 0) {
            consumed = true;
            break;
        }
        furi_timer_stop(s_state.pair_timer);
        s_state.pair_in_progress = false;
        desktop_mesh_clients_set_pairing(desktop->mesh_clients_view, false);

        if(desktop->mesh_pending.accepted) {
            MeshPeer p;
            memcpy(p.mac, desktop->mesh_pending.mac, MESH_MAC_LEN);
            memcpy(p.name, desktop->mesh_pending.name, sizeof(p.name));
            mesh_config_add_client(&p);
            desktop_scene_mesh_clients_refresh_view(desktop);
        }
        consumed = true;
        break;
    }

    case DesktopMeshEventMasterFeatureList:
        /* Live-Status eines Buddys eingetroffen → Cache + Liste aktualisieren. */
        status_apply_featurelist(&desktop->mesh_pending);
        desktop_scene_mesh_clients_refresh_view(desktop);
        consumed = true;
        break;

    default:
        break;
    }

    return consumed;
}

void desktop_scene_mesh_clients_on_exit(void* context) {
    Desktop* desktop = context;

    if(s_state.sweep_timer) {
        furi_timer_stop(s_state.sweep_timer);
        furi_timer_free(s_state.sweep_timer);
        s_state.sweep_timer = NULL;
    }
    if(s_state.pair_timer) {
        furi_timer_stop(s_state.pair_timer);
        furi_timer_free(s_state.pair_timer);
        s_state.pair_timer = NULL;
    }

    /* Handoff zur Action-Scene: Mesh-Service (und eine laufende Capture-Session)
     * weiterlaufen lassen — beim Rückkehr-Enter wird der Service idempotent neu
     * "gestartet" (nur Callback neu gesetzt). */
    if(desktop->mesh_keep_service) {
        return;
    }

    /* Echtes Verlassen des Mesh-Bereichs: nur die lokale Sammel-Session schließen
     * (pcap zu) — der Buddy capturet AUTONOM weiter und wird NICHT gestoppt. Beim
     * Zurückkommen sammelt der Master via Discovery/Auto-Resume wieder ein.
     * Service stoppen (gibt WiFi frei). */
    if(mesh_capture_is_active()) mesh_capture_finish();

    mesh_service_stop();
    if(desktop->mesh_mode == MeshModeClient && !desktop->app_running) {
        mesh_service_start(MeshRoleClient, desktop_mesh_event_cb, desktop);
    }
}
