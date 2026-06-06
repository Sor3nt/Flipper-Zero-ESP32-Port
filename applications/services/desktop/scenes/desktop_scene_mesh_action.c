/**
 * Mesh-Action-Scene (Master) = "Client-Menü": für einen gewählten, gepairten
 * Client die Kategorien Device / Wifi anbieten und unten die aktuell laufende
 * Action ("Idle" / Name) live anzeigen.
 *
 * Der Mesh-Service bleibt über die ganze Mesh-Subtree (Menü → Device/Wifi/
 * Handshake) am Leben — die Clients-Scene hält ihn (desktop->mesh_keep_service).
 * Wir fragen periodisch die Feature-Liste/running_mask des Buddys ab, um das
 * Action-Label aktuell zu halten.
 */

#include <furi.h>
#include <gui/scene_manager.h>
#include <string.h>

#include "../desktop_i.h"
#include "../views/desktop_view_mesh_action.h"
#include "../helpers/mesh_config.h"
#include "../helpers/mesh_service.h"
#include "desktop_scene.h"

#define QUERY_PERIOD_MS 1500

static struct {
    MeshPeer client;
    MeshFeature features[MESH_FEATURES_MAX];
    uint8_t feature_count;
    uint32_t running_mask;
    FuriTimer* query_timer;
} s_state;

/* Action-Label aus running_mask + Feature-Namen ableiten: Capture bevorzugt,
 * sonst erstes laufendes Feature, sonst "Idle". */
static void update_label(Desktop* desktop) {
    const char* chosen = NULL;
    for(uint8_t i = 0; i < s_state.feature_count; ++i) {
        if(!(s_state.running_mask & (1u << s_state.features[i].id))) continue;
        if(strncmp(s_state.features[i].name, "Capture", 7) == 0) {
            chosen = s_state.features[i].name;
            break;
        }
        if(!chosen) chosen = s_state.features[i].name;
    }
    desktop_mesh_action_set_label(desktop->mesh_action_view, chosen ? chosen : "Idle");
}

static void query_tick(void* ctx) {
    (void)ctx;
    mesh_send_feature_query(s_state.client.mac);
}

static void mesh_action_view_cb(DesktopEvent event, void* ctx) {
    Desktop* desktop = ctx;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, event);
}

void desktop_scene_mesh_action_on_enter(void* context) {
    Desktop* desktop = context;

    memset(&s_state, 0, sizeof(s_state));
    s_state.client = desktop->mesh_action_client;

    desktop_mesh_action_set_callback(desktop->mesh_action_view, mesh_action_view_cb, desktop);
    desktop_mesh_action_set_client(desktop->mesh_action_view, s_state.client.name);
    desktop_mesh_action_set_channel(desktop->mesh_action_view, desktop->mesh_action_channel);

    /* In der Clients-Scene bereits geholte Liste/running_mask sofort übernehmen. */
    if(desktop->mesh_action_feature_count > 0) {
        s_state.feature_count = desktop->mesh_action_feature_count > MESH_FEATURES_MAX ?
                                    MESH_FEATURES_MAX :
                                    desktop->mesh_action_feature_count;
        memcpy(
            s_state.features,
            desktop->mesh_action_features,
            s_state.feature_count * sizeof(MeshFeature));
        s_state.running_mask = desktop->mesh_action_running_mask;
    }
    update_label(desktop);

    /* running_mask/Liste laufend auffrischen. */
    mesh_send_feature_query(s_state.client.mac);
    s_state.query_timer = furi_timer_alloc(query_tick, FuriTimerTypePeriodic, desktop);
    furi_timer_start(s_state.query_timer, furi_ms_to_ticks(QUERY_PERIOD_MS));

    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdMeshAction);
}

bool desktop_scene_mesh_action_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = context;
    bool consumed = false;

    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(event.event) {
    case DesktopMeshActionEventDevice:
        scene_manager_next_scene(desktop->scene_manager, DesktopSceneMeshDevice);
        consumed = true;
        break;

    case DesktopMeshActionEventWifi:
        scene_manager_next_scene(desktop->scene_manager, DesktopSceneMeshWifi);
        consumed = true;
        break;

    case DesktopMeshActionEventBack:
        scene_manager_previous_scene(desktop->scene_manager);
        consumed = true;
        break;

    case DesktopMeshEventMasterFeatureList: {
        if(memcmp(desktop->mesh_pending.mac, s_state.client.mac, MESH_MAC_LEN) != 0) {
            consumed = true;
            break;
        }
        s_state.feature_count = desktop->mesh_pending.feature_count > MESH_FEATURES_MAX ?
                                    MESH_FEATURES_MAX :
                                    desktop->mesh_pending.feature_count;
        memcpy(
            s_state.features,
            desktop->mesh_pending.features,
            s_state.feature_count * sizeof(MeshFeature));
        s_state.running_mask = desktop->mesh_pending.running_mask;
        /* Liste/Channel für die Sub-Scenes weiterreichen. */
        desktop->mesh_action_feature_count = s_state.feature_count;
        memcpy(
            desktop->mesh_action_features,
            s_state.features,
            s_state.feature_count * sizeof(MeshFeature));
        desktop->mesh_action_running_mask = s_state.running_mask;
        if(desktop->mesh_pending.rx_channel) {
            desktop->mesh_action_channel = desktop->mesh_pending.rx_channel;
            desktop_mesh_action_set_channel(
                desktop->mesh_action_view, desktop->mesh_action_channel);
        }
        update_label(desktop);
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
            update_label(desktop);
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
}
