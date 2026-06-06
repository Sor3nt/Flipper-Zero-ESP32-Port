/**
 * Mesh-Action-View (Master): Features eines gewählten Clients wählen,
 * konfigurieren (Channel) und starten/stoppen.
 *
 * Layout: Zeile 0 = "Channel: Auto/1..13" (OK cyclet den Wert intern),
 * danach eine Zeile pro vom Client gemeldetem Feature (OK feuert Toggle).
 *
 * Custom-Events (an Scene):
 *   DesktopMeshActionEventToggle  — OK auf einer Feature-Zeile
 *   DesktopMeshActionEventBack    — Back-Taste
 * Die Scene holt sich Selektion/Channel über die get_*-Helfer.
 */
#pragma once

#include <gui/view.h>
#include "desktop_events.h"
#include "../helpers/mesh_service.h" /* MeshFeature / MESH_FEATURES_MAX */

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

/** Feature-Liste (max MESH_FEATURES_MAX) setzen; deaktiviert "loading". */
void desktop_mesh_action_set_features(
    DesktopMeshActionView* view,
    const MeshFeature* features,
    size_t count);

void desktop_mesh_action_set_loading(DesktopMeshActionView* view, bool loading);

/** Bitmask der gerade laufenden Features (Bit i = Feature-ID i läuft → "[stop]").
 *  Mehrere Features können gleichzeitig laufen (z.B. Capture HS + Identify). */
void desktop_mesh_action_set_running_mask(DesktopMeshActionView* view, uint32_t mask);

/** Status-/Footer-Text (z.B. letzter Capture-Summary). NULL = leer. */
void desktop_mesh_action_set_status(DesktopMeshActionView* view, const char* status);

/** Index des selektierten Features (0-basiert), oder -1 wenn die Channel-Zeile
 *  selektiert ist / keine Features da sind. */
int desktop_mesh_action_get_selected_feature(DesktopMeshActionView* view);

/** Aktuell eingestellter Channel (1..13). */
uint8_t desktop_mesh_action_get_channel(DesktopMeshActionView* view);

/** Channel-Wahl von außen setzen — z.B. um nach einem Master-Reboot den
 *  tatsächlichen Capture-Kanal des Buddys (aus dem Status) anzuzeigen. */
void desktop_mesh_action_set_channel(DesktopMeshActionView* view, uint8_t channel);
