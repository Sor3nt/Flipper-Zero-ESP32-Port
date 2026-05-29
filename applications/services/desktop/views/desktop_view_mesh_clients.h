/**
 * Mesh-Clients-View: zeigt im Master-Modus die Liste der gepairten + neu
 * entdeckten Clients und erlaubt pair/remove. Footer zeigt "Discovery" oder
 * "Wait for Accept" während ein Pair-Request raus ist.
 *
 * Datenmodell:
 *   - Die Peers werden vom Scene-Handler über set_peers übergeben (gepairte aus
 *     /ext/mesh/clients.txt + dieser-session discovered ungepairte).
 *   - paired[i] entscheidet ob rechts "[remove]" oder "[pair]" steht und welches
 *     Event beim OK gefeuert wird.
 *
 * Custom-Events (an Scene):
 *   DesktopMeshClientsEventPair    — OK auf einem ungepairten Eintrag
 *   DesktopMeshClientsEventRemove  — OK auf einem gepairten Eintrag
 *   DesktopMeshClientsEventBack    — Back-Taste
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
 *  (leere Liste). Behält den selected_idx wenn möglich, sonst auf 0 geclampt. */
void desktop_mesh_clients_set_peers(
    DesktopMeshClientsView* view,
    const MeshPeer* peers,
    const bool* paired,
    size_t count);

/** Footer "Wait for Accept" (true) / "Discovery" (false). */
void desktop_mesh_clients_set_pairing(DesktopMeshClientsView* view, bool in_progress);

/** Aktuell selektierter Eintrag, -1 wenn Liste leer. */
int desktop_mesh_clients_get_selected_idx(DesktopMeshClientsView* view);
