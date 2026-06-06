/**
 * Mesh-Wifi-Scene (Master): Wifi-Aktionen eines gewählten Clients. Derzeit nur
 * "Capture Handshake" → öffnet die Handshake-Scene. Bewusst als eigene Scene,
 * damit weitere Wifi-Features (Scan, Deauth, …) hier ergänzt werden können.
 */

#include <furi.h>
#include <gui/scene_manager.h>

#include "../desktop_i.h"
#include "../views/desktop_view_mesh_wifi.h"
#include "desktop_scene.h"

static void wifi_view_cb(DesktopEvent event, void* ctx) {
    Desktop* desktop = ctx;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, event);
}

void desktop_scene_mesh_wifi_on_enter(void* context) {
    Desktop* desktop = context;

    desktop_mesh_wifi_set_callback(desktop->mesh_wifi_view, wifi_view_cb, desktop);
    desktop_mesh_wifi_set_client(desktop->mesh_wifi_view, desktop->mesh_action_client.name);
    desktop_mesh_wifi_set_channel(desktop->mesh_wifi_view, desktop->mesh_action_channel);

    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdMeshWifi);
}

bool desktop_scene_mesh_wifi_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = context;
    bool consumed = false;

    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(event.event) {
    case DesktopMeshWifiEventCaptureHs:
        scene_manager_next_scene(desktop->scene_manager, DesktopSceneMeshHandshake);
        consumed = true;
        break;

    case DesktopMeshWifiEventBack:
        scene_manager_previous_scene(desktop->scene_manager);
        consumed = true;
        break;

    default:
        break;
    }
    return consumed;
}

void desktop_scene_mesh_wifi_on_exit(void* context) {
    UNUSED(context);
}
