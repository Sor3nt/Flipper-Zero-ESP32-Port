/**
 * Mesh-Device-Scene (Master): Device-Aktionen eines gewählten Clients —
 * Identify (start/stop) und Disconnect.
 *
 * Identify-Befehle werden wiederholt, bis der Buddy per FeatureStatus bestätigt
 * (er ist beim Capturen oft off-channel und verpasst sonst einen einmaligen
 * Befehl). Die Anzeige (running?) kommt live aus running_mask (FeatureList/Status).
 */

#include <furi.h>
#include <gui/scene_manager.h>
#include <string.h>

#include "../desktop_i.h"
#include "../views/desktop_view_mesh_device.h"
#include "../helpers/mesh_config.h"
#include "../helpers/mesh_service.h"
#include "../helpers/mesh_capture.h"
#include "desktop_scene.h"

#define QUERY_PERIOD_MS 1500
#define CMD_RETRY_MS    500
#define CMD_MAX_TRIES   16

static struct {
    MeshPeer client;
    uint8_t identify_id;
    uint32_t running_mask;
    FuriTimer* query_timer;

    FuriTimer* cmd_timer;
    int pending_feat; /* -1 = keiner */
    bool pending_start;
    uint8_t cmd_tries;
} s_state;

/* Identify-Feature-ID aus der bekannten Liste auflösen (Fallback 0). */
static uint8_t resolve_identify_id(Desktop* desktop) {
    for(uint8_t i = 0; i < desktop->mesh_action_feature_count; ++i) {
        if(strncmp(desktop->mesh_action_features[i].name, "Identify", 8) == 0) {
            return desktop->mesh_action_features[i].id;
        }
    }
    return 0;
}

static void push_running(Desktop* desktop) {
    bool running = (s_state.running_mask & (1u << s_state.identify_id)) != 0;
    desktop_mesh_device_set_identify_running(desktop->mesh_device_view, running);
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
    mesh_send_feature_query(s_state.client.mac);
}

static void device_view_cb(DesktopEvent event, void* ctx) {
    Desktop* desktop = ctx;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, event);
}

void desktop_scene_mesh_device_on_enter(void* context) {
    Desktop* desktop = context;

    memset(&s_state, 0, sizeof(s_state));
    s_state.client = desktop->mesh_action_client;
    s_state.identify_id = resolve_identify_id(desktop);
    s_state.running_mask = desktop->mesh_action_running_mask;
    s_state.pending_feat = -1;

    desktop_mesh_device_set_callback(desktop->mesh_device_view, device_view_cb, desktop);
    desktop_mesh_device_set_client(desktop->mesh_device_view, s_state.client.name);
    desktop_mesh_device_set_channel(desktop->mesh_device_view, desktop->mesh_action_channel);
    push_running(desktop);

    mesh_send_feature_query(s_state.client.mac);
    s_state.query_timer = furi_timer_alloc(query_tick, FuriTimerTypePeriodic, desktop);
    furi_timer_start(s_state.query_timer, furi_ms_to_ticks(QUERY_PERIOD_MS));
    s_state.cmd_timer = furi_timer_alloc(cmd_tick, FuriTimerTypePeriodic, desktop);

    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdMeshDevice);
}

static void start_cmd(Desktop* desktop, bool start) {
    if(start) {
        mesh_send_feature_start(s_state.client.mac, s_state.identify_id, NULL, 0);
    } else {
        mesh_send_feature_stop(s_state.client.mac, s_state.identify_id);
    }
    s_state.pending_feat = s_state.identify_id;
    s_state.pending_start = start;
    s_state.cmd_tries = 0;
    if(s_state.cmd_timer) furi_timer_start(s_state.cmd_timer, furi_ms_to_ticks(CMD_RETRY_MS));
}

bool desktop_scene_mesh_device_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = context;
    bool consumed = false;

    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(event.event) {
    case DesktopMeshDeviceEventIdentifyStart:
        start_cmd(desktop, true);
        consumed = true;
        break;

    case DesktopMeshDeviceEventIdentifyStop:
        start_cmd(desktop, false);
        consumed = true;
        break;

    case DesktopMeshDeviceEventDisconnect: {
        /* Laufende Capture-Session dieses Clients beenden. */
        uint8_t cap_mac[MESH_MAC_LEN];
        if(mesh_capture_is_active() && mesh_capture_get_mac(cap_mac) &&
           memcmp(cap_mac, s_state.client.mac, MESH_MAC_LEN) == 0) {
            mesh_capture_finish();
        }
        mesh_send_disconnect(s_state.client.mac);
        mesh_config_remove_client(s_state.client.mac);
        scene_manager_search_and_switch_to_previous_scene(
            desktop->scene_manager, DesktopSceneMeshClients);
        consumed = true;
        break;
    }

    case DesktopMeshDeviceEventBack:
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
        /* Liste auch für die anderen Sub-Scenes aktuell halten. */
        desktop->mesh_action_feature_count = desktop->mesh_pending.feature_count > MESH_FEATURES_MAX ?
                                                 MESH_FEATURES_MAX :
                                                 desktop->mesh_pending.feature_count;
        memcpy(
            desktop->mesh_action_features,
            desktop->mesh_pending.features,
            desktop->mesh_action_feature_count * sizeof(MeshFeature));
        push_running(desktop);
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
            push_running(desktop);

            /* Ausstehenden Identify-Befehl quittieren. */
            if(s_state.pending_feat == (int)fid) {
                bool done = s_state.pending_start ?
                                (desktop->mesh_pending.feat_state != MeshFeatStateStopped) :
                                (desktop->mesh_pending.feat_state == MeshFeatStateStopped);
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

void desktop_scene_mesh_device_on_exit(void* context) {
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
