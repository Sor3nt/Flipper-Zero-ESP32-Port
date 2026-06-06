/**
 * Mesh-Action-View (Master) = "Client-Menü" für einen gewählten, gepairten
 * Client. Zwei Kategorien (Device / Wifi), darunter zentriert die aktuell
 * laufende Action ("Idle" / Action-Name).
 *
 * Layout:
 *   <Buddy name>            Ch:<channel>
 *   ────────────────────────────────────
 *   Device
 *   Wifi
 *              <Idle/Action>
 *
 * Custom-Events (an Scene):
 *   DesktopMeshActionEventDevice — OK auf "Device"
 *   DesktopMeshActionEventWifi   — OK auf "Wifi"
 *   DesktopMeshActionEventBack   — Back-Taste
 */
#pragma once

#include <gui/view.h>
#include "desktop_events.h"
#include "../helpers/mesh_service.h" /* MESH_NAME_MAX */

typedef struct DesktopMeshActionView DesktopMeshActionView;

typedef void (*DesktopMeshActionViewCallback)(DesktopEvent event, void* context);

DesktopMeshActionView* desktop_mesh_action_alloc(void);
void desktop_mesh_action_free(DesktopMeshActionView* view);
View* desktop_mesh_action_get_view(DesktopMeshActionView* view);

void desktop_mesh_action_set_callback(
    DesktopMeshActionView* view,
    DesktopMeshActionViewCallback callback,
    void* context);

/** Client-Name in den Header setzen. */
void desktop_mesh_action_set_client(DesktopMeshActionView* view, const char* name);

/** Bekannten Kanal des Buddys in den Header setzen (1..13; 0 = unbekannt/aus). */
void desktop_mesh_action_set_channel(DesktopMeshActionView* view, uint8_t channel);

/** Aktuelle Action-Anzeige unten ("Idle" / Action-Name). NULL = "Idle". */
void desktop_mesh_action_set_label(DesktopMeshActionView* view, const char* label);

/** Zentrales Result-Overlay setzen ("Handshake received"). NULL = ausblenden. */
void desktop_mesh_action_set_overlay(DesktopMeshActionView* view, const char* text);
