/**
 * Mesh-Wifi-View (Master): Wifi-Aktionen eines gewählten Clients.
 *
 * Layout:
 *   <Buddy name>            Ch:<channel>
 *   ────────────────────────────────────
 *   Capture Handshake
 *
 * Custom-Events (an Scene):
 *   DesktopMeshWifiEventCaptureHs — OK auf "Capture Handshake"
 *   DesktopMeshWifiEventBack      — Back-Taste
 */
#pragma once

#include <gui/view.h>
#include "desktop_events.h"
#include "../helpers/mesh_service.h" /* MESH_NAME_MAX */

typedef struct DesktopMeshWifiView DesktopMeshWifiView;

typedef void (*DesktopMeshWifiViewCallback)(DesktopEvent event, void* context);

DesktopMeshWifiView* desktop_mesh_wifi_alloc(void);
void desktop_mesh_wifi_free(DesktopMeshWifiView* view);
View* desktop_mesh_wifi_get_view(DesktopMeshWifiView* view);

void desktop_mesh_wifi_set_callback(
    DesktopMeshWifiView* view,
    DesktopMeshWifiViewCallback callback,
    void* context);

void desktop_mesh_wifi_set_client(DesktopMeshWifiView* view, const char* name);
void desktop_mesh_wifi_set_channel(DesktopMeshWifiView* view, uint8_t channel);

/** Zentrales Result-Overlay setzen. NULL = ausblenden. */
void desktop_mesh_wifi_set_overlay(DesktopMeshWifiView* view, const char* text);
