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
#include "desktop_scene.h"

#define TAG "DesktopMeshClients"

#define DISCOVERY_PERIOD_MS 30000
#define PAIR_TIMEOUT_MS     15000

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

    FuriTimer* discovery_timer;
    FuriTimer* pair_timer;

    /* MAC des aktuellen Pair-Requests (für Timeout-Recovery). */
    uint8_t pair_target[MESH_MAC_LEN];
    bool pair_in_progress;
} s_state;

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

    desktop_mesh_clients_set_peers(desktop->mesh_clients_view, s_state.all, s_state.paired, n);
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

static void discovery_tick(void* ctx) {
    (void)ctx;
    mesh_send_discover();
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

    desktop_mesh_clients_set_callback(
        desktop->mesh_clients_view, desktop_scene_mesh_clients_view_cb, desktop);
    desktop_mesh_clients_set_pairing(desktop->mesh_clients_view, false);

    /* Mesh-Service als Master starten. Wenn schon (von einem früheren Re-Enter
     * der Scene) aktiv, ist der Call ein no-op. */
    if(!mesh_service_start(MeshRoleMaster, desktop_mesh_event_cb, desktop)) {
        FURI_LOG_E(TAG, "mesh_service_start failed");
    }

    desktop_scene_mesh_clients_refresh_view(desktop);

    /* Timer anlegen + sofortige erste Discovery senden. */
    s_state.discovery_timer = furi_timer_alloc(discovery_tick, FuriTimerTypePeriodic, desktop);
    s_state.pair_timer = furi_timer_alloc(pair_timeout, FuriTimerTypeOnce, desktop);
    furi_timer_start(s_state.discovery_timer, furi_ms_to_ticks(DISCOVERY_PERIOD_MS));
    mesh_send_discover();

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

    case DesktopMeshClientsEventRemove: {
        int idx = desktop_mesh_clients_get_selected_idx(desktop->mesh_clients_view);
        if(idx < 0 || (size_t)idx >= s_state.all_count) break;
        if(!s_state.paired[idx]) break;

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

    default:
        break;
    }

    return consumed;
}

void desktop_scene_mesh_clients_on_exit(void* context) {
    Desktop* desktop = context;

    if(s_state.discovery_timer) {
        furi_timer_stop(s_state.discovery_timer);
        furi_timer_free(s_state.discovery_timer);
        s_state.discovery_timer = NULL;
    }
    if(s_state.pair_timer) {
        furi_timer_stop(s_state.pair_timer);
        furi_timer_free(s_state.pair_timer);
        s_state.pair_timer = NULL;
    }

    /* Service stoppen — danach evtl. Client-Service wieder hochfahren wenn
     * Modus inzwischen auf Client steht (unwahrscheinlich, aber sauber). */
    mesh_service_stop();
    if(desktop->mesh_mode == MeshModeClient && !desktop->app_running) {
        mesh_service_start(MeshRoleClient, desktop_mesh_event_cb, desktop);
    }
}
