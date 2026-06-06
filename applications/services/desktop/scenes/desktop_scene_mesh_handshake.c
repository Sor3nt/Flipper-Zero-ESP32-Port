/**
 * Mesh-Handshake-Scene (Master): WPA-Handshake-Capture eines Clients steuern.
 * Channel-Auswahl (1..13) + Start/Stop. Der Buddy capturet autonom auf dem
 * gewählten Kanal und streamt die Frames; der Master schreibt die .pcap
 * (mesh_capture, Hintergrund-Session). Ein vollständiger Handshake kommt
 * zusätzlich als zuverlässiges Result ("Handshake received"-Overlay, global).
 */

#include <furi.h>
#include <gui/scene_manager.h>
#include <string.h>

#include "../desktop_i.h"
#include "../views/desktop_view_mesh_handshake.h"
#include "../helpers/mesh_config.h"
#include "../helpers/mesh_service.h"
#include "../helpers/mesh_capture.h"
#include "desktop_scene.h"

#define QUERY_PERIOD_MS 1500

static struct {
    MeshPeer client;
    uint8_t caphs_id;
    uint32_t running_mask;
    FuriTimer* query_timer;
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
    if(s_state.running_mask & (1u << s_state.caphs_id)) return true;
    uint8_t cap_mac[MESH_MAC_LEN];
    return mesh_capture_is_active() && mesh_capture_get_mac(cap_mac) &&
           memcmp(cap_mac, s_state.client.mac, MESH_MAC_LEN) == 0;
}

static void push_capturing(Desktop* desktop) {
    desktop_mesh_handshake_set_capturing(desktop->mesh_handshake_view, capture_running_for_client());
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
    push_capturing(desktop);

    mesh_send_feature_query(s_state.client.mac);
    s_state.query_timer = furi_timer_alloc(query_tick, FuriTimerTypePeriodic, desktop);
    furi_timer_start(s_state.query_timer, furi_ms_to_ticks(QUERY_PERIOD_MS));

    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdMeshHandshake);
}

bool desktop_scene_mesh_handshake_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = context;
    bool consumed = false;

    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(event.event) {
    case DesktopMeshHandshakeEventToggle: {
        if(capture_running_for_client()) {
            mesh_capture_stop();
            /* running-Bit wird beim Stopped-Status gelöscht; Anzeige folgt. */
        } else {
            uint8_t ch = desktop_mesh_handshake_get_capture_channel(desktop->mesh_handshake_view);
            if(mesh_capture_start(s_state.client.mac, s_state.client.name, s_state.caphs_id, ch)) {
                s_state.running_mask |= (1u << s_state.caphs_id);
                desktop->mesh_action_running_mask = s_state.running_mask;
            }
        }
        push_capturing(desktop);
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
        push_capturing(desktop);
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
            push_capturing(desktop);
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
}
