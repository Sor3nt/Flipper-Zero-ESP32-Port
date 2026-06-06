/**
 * Mesh-Handshake-Scene (Master): WPA-Handshake-Capture eines Clients steuern.
 * Channel-Auswahl (1..13) + Start/Stop. Der Buddy capturet autonom auf dem
 * gewählten Kanal und hält die Handshakes durabel; jeder vollständige kommt als
 * zuverlässiges Result ("Handshake received"-Overlay, global) und wird als .pcap
 * geschrieben.
 *
 * Start/Stop bestätigt der Buddy per FeatureStatus. Bis dahin zeigt der Button
 * einen 5-s-Countdown (Pending). Antwortet der Buddy nicht (out of range), läuft
 * der Countdown ab und die Anzeige fällt auf den echten Zustand zurück — so
 * bleibt man nicht in einem falschen "Stop" hängen.
 */

#include <furi.h>
#include <gui/scene_manager.h>
#include <string.h>

#include "../desktop_i.h"
#include "../views/desktop_view_mesh_handshake.h"
#include "../helpers/mesh_config.h"
#include "../helpers/mesh_service.h"
#include "desktop_scene.h"

#define QUERY_PERIOD_MS 1500
#define HS_CONFIRM_SECS 5 /* Countdown, bis der Buddy Start/Stop bestätigt haben muss */

static struct {
    MeshPeer client;
    uint8_t caphs_id;
    uint32_t running_mask;
    FuriTimer* query_timer;

    /* Pending: auf FeatureStatus-Bestätigung warten (Button zeigt Countdown). */
    bool pending;
    bool pending_start; /* true: Start erwartet (running), false: Stop (stopped) */
    uint8_t pending_secs;
    FuriTimer* confirm_timer;
} s_state;

/* Capture-HS-Feature-ID aus der Liste auflösen (Namenspräfix "Capture", Fallback 2). */
static uint8_t resolve_caphs_id(Desktop* desktop) {
    for(uint8_t i = 0; i < desktop->mesh_action_feature_count; ++i) {
        if(strncmp(desktop->mesh_action_features[i].name, "Capture", 7) == 0) {
            return desktop->mesh_action_features[i].id;
        }
    }
    return 2;
}

static bool capture_running_for_client(void) {
    /* Quelle der Wahrheit ist die running_mask des Buddys (live abgefragt). */
    return (s_state.running_mask & (1u << s_state.caphs_id)) != 0;
}

/* Button-Anzeige aktualisieren: Pending (Countdown) hat Vorrang, sonst Stop/Start. */
static void refresh_button(Desktop* desktop) {
    desktop_mesh_handshake_set_pending(
        desktop->mesh_handshake_view, s_state.pending, s_state.pending_secs);
    desktop_mesh_handshake_set_capturing(desktop->mesh_handshake_view, capture_running_for_client());
}

/* Pending beenden, wenn der erwartete Zustand erreicht ist. */
static void confirm_if_done(Desktop* desktop) {
    if(!s_state.pending) return;
    bool running = capture_running_for_client();
    bool done = s_state.pending_start ? running : !running;
    if(done) {
        s_state.pending = false;
        if(s_state.confirm_timer) furi_timer_stop(s_state.confirm_timer);
    }
}

static void confirm_tick(void* ctx) {
    Desktop* desktop = ctx;
    if(!s_state.pending) return;
    if(s_state.pending_secs <= 1) {
        /* Timeout: keine Bestätigung (Buddy out of range?) → Wartezustand beenden,
         * Anzeige fällt auf den echten Zustand zurück. */
        s_state.pending = false;
        if(s_state.confirm_timer) furi_timer_stop(s_state.confirm_timer);
        refresh_button(desktop);
        return;
    }
    s_state.pending_secs--;
    refresh_button(desktop);
}

static void query_tick(void* ctx) {
    (void)ctx;
    mesh_send_feature_query(s_state.client.mac);
}

static void handshake_view_cb(DesktopEvent event, void* ctx) {
    Desktop* desktop = ctx;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, event);
}

void desktop_scene_mesh_handshake_on_enter(void* context) {
    Desktop* desktop = context;

    memset(&s_state, 0, sizeof(s_state));
    s_state.client = desktop->mesh_action_client;
    s_state.caphs_id = resolve_caphs_id(desktop);
    s_state.running_mask = desktop->mesh_action_running_mask;

    desktop_mesh_handshake_set_callback(desktop->mesh_handshake_view, handshake_view_cb, desktop);
    desktop_mesh_handshake_set_client(desktop->mesh_handshake_view, s_state.client.name);
    desktop_mesh_handshake_set_channel(desktop->mesh_handshake_view, desktop->mesh_action_channel);
    uint8_t def_ch = (desktop->mesh_action_channel >= 1 && desktop->mesh_action_channel <= 13) ?
                         desktop->mesh_action_channel :
                         1;
    desktop_mesh_handshake_set_capture_channel(desktop->mesh_handshake_view, def_ch);
    refresh_button(desktop);

    mesh_send_feature_query(s_state.client.mac);
    s_state.query_timer = furi_timer_alloc(query_tick, FuriTimerTypePeriodic, desktop);
    furi_timer_start(s_state.query_timer, furi_ms_to_ticks(QUERY_PERIOD_MS));
    s_state.confirm_timer = furi_timer_alloc(confirm_tick, FuriTimerTypePeriodic, desktop);

    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdMeshHandshake);
}

bool desktop_scene_mesh_handshake_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = context;
    bool consumed = false;

    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(event.event) {
    case DesktopMeshHandshakeEventToggle: {
        if(s_state.pending) {
            consumed = true; /* schon am Warten — Doppeldruck ignorieren */
            break;
        }
        bool running = capture_running_for_client();
        if(running) {
            mesh_send_feature_stop(s_state.client.mac, s_state.caphs_id);
            s_state.pending_start = false;
        } else {
            uint8_t ch = desktop_mesh_handshake_get_capture_channel(desktop->mesh_handshake_view);
            mesh_send_feature_start(s_state.client.mac, s_state.caphs_id, &ch, 1);
            /* Start geht auf dem aktuellen (Idle-)Kanal des Buddys raus, dann folgen
             * wir ihm auf den Capture-Kanal (ESP-NOW + Monitor parallel). */
            if(ch >= 1 && ch <= 13) mesh_service_set_channel(ch);
            s_state.pending_start = true;
        }
        /* NICHT optimistisch als laufend markieren — erst auf die FeatureStatus-
         * Bestätigung warten (Countdown). Bleibt sie aus → Timeout → Rückfall. */
        s_state.pending = true;
        s_state.pending_secs = HS_CONFIRM_SECS;
        furi_timer_start(s_state.confirm_timer, furi_ms_to_ticks(1000));
        refresh_button(desktop);
        consumed = true;
        break;
    }

    case DesktopMeshHandshakeEventBack:
        /* Laufender Capture bleibt aktiv (Hintergrund-Session). */
        scene_manager_previous_scene(desktop->scene_manager);
        consumed = true;
        break;

    case DesktopMeshEventMasterFeatureList: {
        if(memcmp(desktop->mesh_pending.mac, s_state.client.mac, MESH_MAC_LEN) != 0) {
            consumed = true;
            break;
        }
        s_state.running_mask = desktop->mesh_pending.running_mask;
        desktop->mesh_action_running_mask = s_state.running_mask;
        if(desktop->mesh_pending.rx_channel) {
            desktop->mesh_action_channel = desktop->mesh_pending.rx_channel;
            desktop_mesh_handshake_set_channel(
                desktop->mesh_handshake_view, desktop->mesh_action_channel);
        }
        confirm_if_done(desktop);
        refresh_button(desktop);
        consumed = true;
        break;
    }

    case DesktopMeshEventMasterFeatureStatus: {
        if(memcmp(desktop->mesh_pending.mac, s_state.client.mac, MESH_MAC_LEN) != 0) {
            consumed = true;
            break;
        }
        uint8_t fid = desktop->mesh_pending.feat_id;
        if(fid < 32) {
            if(desktop->mesh_pending.feat_state == MeshFeatStateStopped) {
                s_state.running_mask &= ~(1u << fid);
            } else {
                s_state.running_mask |= (1u << fid);
            }
            desktop->mesh_action_running_mask = s_state.running_mask;
            confirm_if_done(desktop);
            refresh_button(desktop);
        }
        consumed = true;
        break;
    }

    default:
        break;
    }
    return consumed;
}

void desktop_scene_mesh_handshake_on_exit(void* context) {
    Desktop* desktop = context;
    UNUSED(desktop);
    if(s_state.query_timer) {
        furi_timer_stop(s_state.query_timer);
        furi_timer_free(s_state.query_timer);
        s_state.query_timer = NULL;
    }
    if(s_state.confirm_timer) {
        furi_timer_stop(s_state.confirm_timer);
        furi_timer_free(s_state.confirm_timer);
        s_state.confirm_timer = NULL;
    }
    s_state.pending = false;
}
