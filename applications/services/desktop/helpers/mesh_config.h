/**
 * Mesh-Config: persistente State-Files auf der SD-Karte.
 *
 *   /ext/mesh/clients.txt    eine Zeile pro gepairtem Client: "AA:BB:CC:DD:EE:FF;Name"
 *   /ext/mesh/master.txt     einzig gepairter Master:        "AA:BB:CC:DD:EE:FF;Name"
 *
 * Alle Funktionen sind synchronous, öffnen den Storage-Service via
 * furi_record_open(RECORD_STORAGE) und schließen ihn wieder. Konstruktion und
 * Verzeichnis-Anlage ist idempotent; fehlende Files bedeuten "leerer State".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_NAME_MAX     32  // ohne NUL
#define MESH_MAC_LEN      6
#define MESH_CLIENTS_MAX  16  // ESP-NOW erlaubt 20 unicast peers — wir bleiben drunter

typedef struct {
    uint8_t mac[MESH_MAC_LEN];
    char name[MESH_NAME_MAX + 1];
} MeshPeer;

/* clients.txt — out_count wird mit der Anzahl gelesener Einträge gesetzt
 * (max MESH_CLIENTS_MAX). Gibt false zurück nur bei harten IO-Fehlern. */
bool mesh_config_load_clients(MeshPeer* out, size_t* out_count);
bool mesh_config_save_clients(const MeshPeer* peers, size_t count);
bool mesh_config_add_client(const MeshPeer* peer);    // dedupliziert per MAC
bool mesh_config_remove_client(const uint8_t mac[MESH_MAC_LEN]);

/* master.txt — false wenn nicht vorhanden. */
bool mesh_config_load_master(MeshPeer* out);
bool mesh_config_save_master(const MeshPeer* peer);
void mesh_config_clear_master(void);

/* Hilfsfunktionen für MAC-Format. */
void mesh_mac_to_str(const uint8_t mac[MESH_MAC_LEN], char out[18]);
bool mesh_mac_from_str(const char* s, uint8_t out[MESH_MAC_LEN]);

#ifdef __cplusplus
}
#endif
