/**
 * Mesh-Handshake-View (Master): WPA-Handshake-Capture eines Clients steuern.
 *
 * Layout:
 *   <Buddy name>            Ch:<channel>
 *   ────────────────────────────────────
 *   Channel              <1-13>
 *            [ Start/Stop ]
 *
 * Channel ist eine Wert-Zeile (OK → Edit, drehen 1..13, OK bestätigt — gesendet
 * wird erst bei Start). Darunter ein zentrierter Start/Stop-Button.
 *
 * Custom-Events (an Scene):
 *   DesktopMeshHandshakeEventToggle — OK auf dem Button → capture start/stop
 *   DesktopMeshHandshakeEventBack   — Back-Taste (nicht im Edit-Modus)
 */
#pragma once

#include <gui/view.h>
#include "desktop_events.h"
#include "../helpers/mesh_service.h" /* MESH_NAME_MAX */

typedef struct DesktopMeshHandshakeView DesktopMeshHandshakeView;

typedef void (*DesktopMeshHandshakeViewCallback)(DesktopEvent event, void* context);

DesktopMeshHandshakeView* desktop_mesh_handshake_alloc(void);
void desktop_mesh_handshake_free(DesktopMeshHandshakeView* view);
View* desktop_mesh_handshake_get_view(DesktopMeshHandshakeView* view);

void desktop_mesh_handshake_set_callback(
    DesktopMeshHandshakeView* view,
    DesktopMeshHandshakeViewCallback callback,
    void* context);

void desktop_mesh_handshake_set_client(DesktopMeshHandshakeView* view, const char* name);

/** Bekannter Buddy-Kanal für den Header (1..13; 0 = unbekannt). */
void desktop_mesh_handshake_set_channel(DesktopMeshHandshakeView* view, uint8_t channel);

/** Vorausgewählten Capture-Kanal setzen (1..13). */
void desktop_mesh_handshake_set_capture_channel(DesktopMeshHandshakeView* view, uint8_t channel);

/** Aktuell ausgewählter Capture-Kanal (1..13). */
uint8_t desktop_mesh_handshake_get_capture_channel(DesktopMeshHandshakeView* view);

/** Läuft der Capture gerade? — Button zeigt dann "Stop". */
void desktop_mesh_handshake_set_capturing(DesktopMeshHandshakeView* view, bool capturing);

/** Zentrales Result-Overlay setzen. NULL = ausblenden. */
void desktop_mesh_handshake_set_overlay(DesktopMeshHandshakeView* view, const char* text);
