/**
 * Mesh-Action-Scene (Master): für einen gewählten, gepairten Client dessen
 * Features anzeigen, konfigurieren (Channel) und starten/stoppen.
 *
 * Beim Enter wird der Mesh-Service NICHT neu gestartet — die Clients-Scene hält
 * ihn am Leben (desktop->mesh_keep_service handoff). Wir fragen die Feature-Liste
 * des Clients ab (FeatureQuery, mit Retry bis Antwort) und füllen die View.
 *
 * Capture-HS-Feature (Name beginnt mit "Capture") läuft über mesh_capture
 * (pcap-Streaming auf SD, Hintergrund-Session). Andere Features (Identify, Echo)
 * werden direkt per mesh_send_feature_start/stop gesteuert.
 */

#include <furi.h>
#include <gui/scene_manager.h>
#include <string.h>

#include "../desktop_i.h"
#include "../views/desktop_view_mesh_action.h"
#include "../helpers/mesh_config.h"
#include "../helpers/mesh_service.h"
#include "../helpers/mesh_capture.h"
#include "desktop_scene.h"

#define TAG "DesktopMeshAction"

#define QUERY_RETRY_MS 1500
#define CMD_RETRY_MS   500
#define CMD_MAX_TRIES  16 /* ~8s — der Buddy ist beim Capturen nur ~18% auf ch1 */

static struct {
    MeshPeer client;
    MeshFeature features[MESH_FEATURES_MAX];
    uint8_t feature_count;
    uint32_t running_mask; /* Bit i = Feature-ID i läuft (mehrere parallel möglich) */
    FuriTimer* query_timer;

    /* Generischer Feature-Befehl (Start/Stop) wird wiederholt, bis der Buddy per
     * FeatureStatus bestätigt — nötig, weil er beim Capturen meist off-channel
     * ist und einen einmaligen Befehl sonst verpasst. */
    FuriTimer* cmd_timer;
    int pending_feat; /* -1 = keiner */
    bool pending_start; /* true: Start (erwartet running), false: Stop (erwartet stopped) */
    uint8_t cmd_tries;
} s_state;

static bool is_capture_feature(const char* name) {
    return strncmp(name, "Capture", 7) == 0;
}

static void cmd_tick(void* ctx) {
    (void)ctx;
    if(s_state.pending_feat < 0) return;
    if(++s_state.cmd_tries > CMD_MAX_TRIES) {
        s_state.pending_feat = -1;
        if(s_state.cmd_timer) furi_timer_stop(s_state.cmd_timer);
        return;
    }
    if(s_state.pending_start) {
        mesh_send_feature_start(s_state.client.mac, (uint8_t)s_state.pending_feat, NULL, 0);
    } else {
        mesh_send_feature_stop(s_state.client.mac, (uint8_t)s_state.pending_feat);
    }
}

static void query_tick(void* ctx) {
    (void)ctx;
    /* Solange noch keine Liste da ist: erneut anfragen (Buddy ist evtl. gerade
     * off-channel beim Capturen). */
    if(s_state.feature_count == 0) mesh_send_feature_query(s_state.client.mac);
}

static void mesh_action_view_cb(DesktopEvent event, void* ctx) {
    Desktop* desktop = ctx;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, event);
}

void desktop_scene_mesh_action_on_enter(void* context) {
    Desktop* desktop = context;

    memset(&s_state, 0, sizeof(s_state));
    s_state.client = desktop->mesh_action_client;
    s_state.pending_feat = -1;
    s_state.running_mask = 0;

    desktop_mesh_action_set_callback(desktop->mesh_action_view, mesh_action_view_cb, desktop);
    desktop_mesh_action_set_client(desktop->mesh_action_view, s_state.client.name);

    /* Bereits in der Clients-Scene geholte Feature-Liste sofort anzeigen (kein
     * zweites Warten). Der running_mask wird unten per Query nochmal aufgefrischt. */
    if(desktop->mesh_action_feature_count > 0) {
        s_state.feature_count = desktop->mesh_action_feature_count > MESH_FEATURES_MAX ?
                                    MESH_FEATURES_MAX :
                                    desktop->mesh_action_feature_count;
        memcpy(
            s_state.features,
            desktop->mesh_action_features,
            s_state.feature_count * sizeof(MeshFeature));
        s_state.running_mask = desktop->mesh_action_running_mask;
        desktop_mesh_action_set_features(
            desktop->mesh_action_view, s_state.features, s_state.feature_count);
    } else {
        desktop_mesh_action_set_loading(desktop->mesh_action_view, true);
    }
    desktop_mesh_action_set_running_mask(desktop->mesh_action_view, s_state.running_mask);
    desktop_mesh_action_set_status(desktop->mesh_action_view, NULL);

    /* Trotzdem einmal abfragen, um running_mask aufzufrischen (Retry-Timer nur
     * solange noch keine Liste da ist). */
    mesh_send_feature_query(s_state.client.mac);
    s_state.query_timer = furi_timer_alloc(query_tick, FuriTimerTypePeriodic, desktop);
    furi_timer_start(s_state.query_timer, furi_ms_to_ticks(QUERY_RETRY_MS));
    s_state.cmd_timer = furi_timer_alloc(cmd_tick, FuriTimerTypePeriodic, desktop);

    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdMeshAction);
}

static void handle_toggle(Desktop* desktop) {
    int idx = desktop_mesh_action_get_selected_feature(desktop->mesh_action_view);
    if(idx < 0 || (size_t)idx >= s_state.feature_count) return;
    MeshFeature f = s_state.features[idx];
    bool running = (s_state.running_mask & (1u << f.id)) != 0;

    if(is_capture_feature(f.name)) {
        if(running) {
            mesh_capture_stop();
            desktop_mesh_action_set_status(desktop->mesh_action_view, "stopping...");
            /* running-Bit wird beim Stopped-Status gelöscht */
        } else {
            uint8_t ch = desktop_mesh_action_get_channel(desktop->mesh_action_view);
            if(mesh_capture_start(s_state.client.mac, s_state.client.name, f.id, ch)) {
                s_state.running_mask |= (1u << f.id);
                desktop_mesh_action_set_running_mask(desktop->mesh_action_view, s_state.running_mask);
                desktop_mesh_action_set_status(desktop->mesh_action_view, "capturing...");
            } else {
                desktop_mesh_action_set_status(desktop->mesh_action_view, "pcap open failed");
            }
        }
    } else {
        /* Generisches Feature (z.B. Identify): Befehl wiederholen, bis der Buddy
         * bestätigt — er ist beim Capturen meist off-channel und verpasst sonst
         * einen einmaligen Befehl. */
        if(running) {
            mesh_send_feature_stop(s_state.client.mac, f.id);
            s_state.pending_feat = f.id;
            s_state.pending_start = false;
            desktop_mesh_action_set_status(desktop->mesh_action_view, "stopping...");
        } else {
            mesh_send_feature_start(s_state.client.mac, f.id, NULL, 0);
            /* NICHT optimistisch als laufend markieren — das Bit wird erst gesetzt,
             * wenn der Buddy per FeatureStatus(Running) bestätigt. Sonst zeigt die
             * Zeile sofort "[stop]" und ein zweiter Klick stoppt, bevor der Start
             * überhaupt angekommen ist. */
            s_state.pending_feat = f.id;
            s_state.pending_start = true;
            desktop_mesh_action_set_status(desktop->mesh_action_view, "starting...");
        }
        s_state.cmd_tries = 0;
        if(s_state.cmd_timer) furi_timer_start(s_state.cmd_timer, furi_ms_to_ticks(CMD_RETRY_MS));
    }
}

bool desktop_scene_mesh_action_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = context;
    bool consumed = false;

    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(event.event) {
    case DesktopMeshActionEventToggle:
        handle_toggle(desktop);
        consumed = true;
        break;

    case DesktopMeshActionEventBack:
        /* Laufende Actions bleiben aktiv (Capture läuft als BG-Session weiter). */
        scene_manager_previous_scene(desktop->scene_manager);
        consumed = true;
        break;

    case DesktopMeshEventMasterFeatureList: {
        if(memcmp(desktop->mesh_pending.mac, s_state.client.mac, MESH_MAC_LEN) != 0) {
            consumed = true;
            break;
        }
        s_state.feature_count = desktop->mesh_pending.feature_count;
        if(s_state.feature_count > MESH_FEATURES_MAX) s_state.feature_count = MESH_FEATURES_MAX;
        memcpy(
            s_state.features,
            desktop->mesh_pending.features,
            s_state.feature_count * sizeof(MeshFeature));
        desktop_mesh_action_set_features(
            desktop->mesh_action_view, s_state.features, s_state.feature_count);
        /* Laufende Features kommen live mit (running_mask) — Quelle Buddy. */
        s_state.running_mask = desktop->mesh_pending.running_mask;
        desktop_mesh_action_set_running_mask(desktop->mesh_action_view, s_state.running_mask);
        if(s_state.query_timer) furi_timer_stop(s_state.query_timer);

        /* Auto-Resume: läuft ein Capture (z.B. nach Master-Reboot) und wir sammeln
         * noch nicht → anhängen, damit Stop wirkt und Ergebnisse gesammelt werden. */
        if(!mesh_capture_is_active()) {
            uint8_t ch = desktop->mesh_pending.rx_channel ? desktop->mesh_pending.rx_channel :
                                                            desktop->mesh_action_channel;
            for(uint8_t i = 0; i < s_state.feature_count; ++i) {
                if((s_state.running_mask & (1u << s_state.features[i].id)) &&
                   is_capture_feature(s_state.features[i].name)) {
                    mesh_capture_attach(
                        s_state.client.mac, s_state.client.name, s_state.features[i].id, ch);
                    break;
                }
            }
        }
        consumed = true;
        break;
    }

    case DesktopMeshEventMasterFeatureStatus: {
        if(memcmp(desktop->mesh_pending.mac, s_state.client.mac, MESH_MAC_LEN) != 0) {
            consumed = true;
            break;
        }
        uint8_t fid = desktop->mesh_pending.feat_id;
        uint8_t st = desktop->mesh_pending.feat_state;
        const char* txt = (const char*)desktop->mesh_pending.feat_data; /* NUL-terminiert */
        if(fid < 32) {
            if(st == MeshFeatStateStopped) {
                s_state.running_mask &= ~(1u << fid);
            } else {
                s_state.running_mask |= (1u << fid);
                if(desktop->mesh_pending.feat_data_len) {
                    desktop_mesh_action_set_status(desktop->mesh_action_view, txt);
                    /* Capture-Status ist "ch<N> ..." — den echten Kanal in die
                     * Channel-Zeile übernehmen (nach Master-Reboot sonst Default 1). */
                    if(txt[0] == 'c' && txt[1] == 'h' && txt[2] >= '0' && txt[2] <= '9') {
                        int ch = txt[2] - '0';
                        if(txt[3] >= '0' && txt[3] <= '9') ch = ch * 10 + (txt[3] - '0');
                        if(ch >= 1 && ch <= 13)
                            desktop_mesh_action_set_channel(
                                desktop->mesh_action_view, (uint8_t)ch);
                    }
                }
            }
            desktop_mesh_action_set_running_mask(desktop->mesh_action_view, s_state.running_mask);

            /* Ausstehenden Befehl quittieren, wenn der erwartete Zustand erreicht ist. */
            if(s_state.pending_feat == (int)fid) {
                bool done = s_state.pending_start ? (st != MeshFeatStateStopped) :
                                                    (st == MeshFeatStateStopped);
                if(done) {
                    s_state.pending_feat = -1;
                    if(s_state.cmd_timer) furi_timer_stop(s_state.cmd_timer);
                }
            }
        }
        consumed = true;
        break;
    }

    default:
        break;
    }
    return consumed;
}

void desktop_scene_mesh_action_on_exit(void* context) {
    Desktop* desktop = context;
    UNUSED(desktop);
    if(s_state.query_timer) {
        furi_timer_stop(s_state.query_timer);
        furi_timer_free(s_state.query_timer);
        s_state.query_timer = NULL;
    }
    if(s_state.cmd_timer) {
        furi_timer_stop(s_state.cmd_timer);
        furi_timer_free(s_state.cmd_timer);
        s_state.cmd_timer = NULL;
    }
    s_state.pending_feat = -1;
}
