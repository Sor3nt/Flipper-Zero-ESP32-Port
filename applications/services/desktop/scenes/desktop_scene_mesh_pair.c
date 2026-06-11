/**
 * Mesh-Pair-Scene (Client): zeigt den Pair-Confirm-Dialog wenn der Background-
 * Client-Mesh-Service einen PairRequest empfangen hat.
 *
 * Eingabe: desktop->mesh_pending wurde vom Service mit Master-MAC und -Namen
 * befüllt; Main-Scene hat per scene_manager_next_scene zu uns navigiert.
 *
 * Aktionen:
 *   - Right (Yes)  → mesh_send_pair_response(true), master.txt schreiben
 *   - Left  (No)   → mesh_send_pair_response(false), master.txt nicht ändern
 *   - Back         → wie No (silent decline)
 *
 * Danach zurück zur Main-Scene.
 */

#include <furi.h>
#include <gui/modules/dialog_ex.h>
#include <string.h>
#include <stdio.h>

#include "../desktop_i.h"
#include "../helpers/mesh_config.h"
#include "../helpers/mesh_service.h"
#include "desktop_scene.h"

#define TAG "DesktopMeshPair"

/* Lokal gehaltener Snapshot der relevanten Pending-Werte. Wir kopieren beim
 * Enter, weil der Service danach den Slot überschreiben könnte. */
static struct {
    uint8_t mac[MESH_MAC_LEN];
    char name[MESH_NAME_MAX + 1];
    char header[64];
} s_pair;

static void mesh_pair_result_callback(DialogExResult result, void* context) {
    Desktop* desktop = context;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, (uint32_t)result);
}

void desktop_scene_mesh_pair_on_enter(void* context) {
    Desktop* desktop = context;

    memcpy(s_pair.mac, desktop->mesh_pending.mac, MESH_MAC_LEN);
    strncpy(s_pair.name, desktop->mesh_pending.name, MESH_NAME_MAX);
    s_pair.name[MESH_NAME_MAX] = '\0';
    snprintf(s_pair.header, sizeof(s_pair.header), "Pair with\n%s?", s_pair.name);

    DialogEx* d = desktop->mesh_pair_dialog;
    dialog_ex_reset(d);
    dialog_ex_set_header(d, "Mesh Pair", 64, 8, AlignCenter, AlignTop);
    dialog_ex_set_text(d, s_pair.header, 64, 32, AlignCenter, AlignCenter);
    dialog_ex_set_left_button_text(d, "No");
    dialog_ex_set_right_button_text(d, "Yes");
    dialog_ex_set_context(d, desktop);
    dialog_ex_set_result_callback(d, mesh_pair_result_callback);

    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdMeshPair);
}

bool desktop_scene_mesh_pair_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeBack) {
        /* Wie No (silent). */
        mesh_send_pair_response(s_pair.mac, false);
        scene_manager_search_and_switch_to_previous_scene(desktop->scene_manager, DesktopSceneMain);
        return true;
    }

    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == DialogExResultRight) {
        /* Yes: master persistieren und Response senden. */
        MeshPeer master = {0};
        memcpy(master.mac, s_pair.mac, MESH_MAC_LEN);
        strncpy(master.name, s_pair.name, MESH_NAME_MAX);
        master.name[MESH_NAME_MAX] = '\0';
        mesh_config_save_master(&master);
        mesh_send_pair_response(s_pair.mac, true);
        scene_manager_search_and_switch_to_previous_scene(desktop->scene_manager, DesktopSceneMain);
        consumed = true;
    } else if(event.event == DialogExResultLeft) {
        mesh_send_pair_response(s_pair.mac, false);
        scene_manager_search_and_switch_to_previous_scene(desktop->scene_manager, DesktopSceneMain);
        consumed = true;
    }
    return consumed;
}

void desktop_scene_mesh_pair_on_exit(void* context) {
    Desktop* desktop = context;
    dialog_ex_reset(desktop->mesh_pair_dialog);
}
