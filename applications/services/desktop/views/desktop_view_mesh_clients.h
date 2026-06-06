/**
 * Mesh-Clients-View: zeigt im Master-Modus die Liste der gepairten + neu
 * entdeckten Clients und erlaubt pair/remove. Footer zeigt "Wait for Accept"
 * während ein Pair-Request raus ist (sonst kein Footer).
 *
 * Datenmodell:
 *   - Die Peers werden vom Scene-Handler über set_peers übergeben (gepairte aus
 *     /ext/mesh/clients.txt + dieser-session discovered ungepairte).
 *   - paired[i] entscheidet ob rechts "[remove]" oder "[pair]" steht und welches
 *     Event beim OK gefeuert wird.
 *
 * Custom-Events (an Scene):
 *   DesktopMeshClientsEventPair        — OK kurz auf ungepairtem Eintrag
 *   DesktopMeshClientsEventOpenAction  — OK kurz auf gepairtem Eintrag
 *   DesktopMeshClientsEventRemove      — OK lang auf gepairtem Eintrag
 *   DesktopMeshClientsEventBack        — Back-Taste
 *
 * Nach einem Event holt die Scene den Selected-Index via get_selected_idx().
 */
#pragma once

#include <gui/view.h>
#include "desktop_events.h"
#include "../helpers/mesh_config.h" /* MeshPeer / MESH_CLIENTS_MAX */

typedef struct DesktopMeshClientsView DesktopMeshClientsView;

typedef void (*DesktopMeshClientsViewCallback)(DesktopEvent event, void* context);

DesktopMeshClientsView* desktop_mesh_clients_alloc(void);
void desktop_mesh_clients_free(DesktopMeshClientsView* view);
View* desktop_mesh_clients_get_view(DesktopMeshClientsView* view);

void desktop_mesh_clients_set_callback(
    DesktopMeshClientsView* view,
    DesktopMeshClientsViewCallback callback,
    void* context);

/** Liste in den View kopieren (max MESH_CLIENTS_MAX). count==0 ist erlaubt
 *  (leere Liste). Behält den selected_idx wenn möglich, sonst auf 0 geclampt.
 *  status[i]: Label für gepairte Einträge ("Idle" oder Action-Name); NULL/leer
 *  ⇒ "Idle". Für ungepairte Einträge ignoriert (zeigt "[pair]"). status darf
 *  selbst NULL sein. */
void desktop_mesh_clients_set_peers(
    DesktopMeshClientsView* view,
    const MeshPeer* peers,
    const bool* paired,
    const char* const* status,
    size_t count);

/** Footer "Wait for Accept" einblenden (true) / ausblenden (false). */
void desktop_mesh_clients_set_pairing(DesktopMeshClientsView* view, bool in_progress);

/** Zentrales Result-Overlay setzen ("Handshake received"). NULL = ausblenden. */
void desktop_mesh_clients_set_overlay(DesktopMeshClientsView* view, const char* text);

/** Aktuell selektierter Eintrag, -1 wenn Liste leer. */
int desktop_mesh_clients_get_selected_idx(DesktopMeshClientsView* view);
