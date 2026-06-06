/**
 * Mesh-Device-View (Master): Device-Aktionen eines gewählten Clients.
 *
 * Layout:
 *   <Buddy name>            Ch:<channel>
 *   ────────────────────────────────────
 *   Identify             <start/stop>
 *   Disconnect
 *
 * Identify ist eine Wert-Zeile (OK → Edit, drehen wechselt start/stop, OK
 * bestätigt → Scene sendet feature start/stop). Disconnect ist eine Action-Zeile.
 *
 * Custom-Events (an Scene):
 *   DesktopMeshDeviceEventIdentifyStart — Edit bestätigt mit "start"
 *   DesktopMeshDeviceEventIdentifyStop  — Edit bestätigt mit "stop"
 *   DesktopMeshDeviceEventDisconnect    — OK auf "Disconnect"
 *   DesktopMeshDeviceEventBack          — Back-Taste (nicht im Edit-Modus)
 */
#pragma once

#include <gui/view.h>
#include "desktop_events.h"
#include "../helpers/mesh_service.h" /* MESH_NAME_MAX */

typedef struct DesktopMeshDeviceView DesktopMeshDeviceView;

typedef void (*DesktopMeshDeviceViewCallback)(DesktopEvent event, void* context);

DesktopMeshDeviceView* desktop_mesh_device_alloc(void);
void desktop_mesh_device_free(DesktopMeshDeviceView* view);
View* desktop_mesh_device_get_view(DesktopMeshDeviceView* view);

void desktop_mesh_device_set_callback(
    DesktopMeshDeviceView* view,
    DesktopMeshDeviceViewCallback callback,
    void* context);

void desktop_mesh_device_set_client(DesktopMeshDeviceView* view, const char* name);
void desktop_mesh_device_set_channel(DesktopMeshDeviceView* view, uint8_t channel);

/** Läuft Identify gerade? (Quelle: running_mask des Buddys) — steuert die Anzeige. */
void desktop_mesh_device_set_identify_running(DesktopMeshDeviceView* view, bool running);

/** Zentrales Result-Overlay setzen. NULL = ausblenden. */
void desktop_mesh_device_set_overlay(DesktopMeshDeviceView* view, const char* text);
