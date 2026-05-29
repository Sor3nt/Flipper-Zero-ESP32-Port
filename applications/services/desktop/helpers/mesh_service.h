/**
 * Mesh-Service: WiFi + ESP-NOW gekapselt für die Mesh-Funktionalität (Phase 1).
 *
 * Threading: ESP-NOW / esp_wifi-APIs brauchen einen echten FreeRTOS-Task (kein
 * FuriThread — siehe nrf24_wifi_scan.c und CLAUDE.md feedback_furi_thread_lwip).
 * Der Service startet daher intern einen xTaskCreate-Worker, der WiFi/ESP-NOW
 * initialisiert und auf einer Command-Queue blockiert. Die public Sende-APIs
 * sind reine Producer (kein eigener WiFi-Call) und damit aus jedem Kontext
 * sicher aufrufbar — auch aus FuriThreads/Scenes.
 *
 * Empfangene Pakete werden im WiFi-Internal-Task entgegengenommen, vom Service
 * geparst, und der User-Callback wird im selben Kontext aufgerufen. Üblicher
 * User-Callback-Inhalt: view_dispatcher_send_custom_event (intern thread-safe).
 *
 * Auto-Reply: Im Client-Modus antwortet der Service intern auf Discover-Pakete
 * (DiscoverResponse) und auf Disconnect (DisconnectAck) — die Scene muss sich
 * darum nicht kümmern. Bei Pair-Requests wird der Callback gefeuert; die Scene
 * muss explizit mesh_send_pair_response() aufrufen.
 *
 * WiFi-Channel: fest auf 1 (ESP-NOW braucht für broadcasts einen einheitlichen
 * Kanal; alle Peers im Mesh müssen denselben benutzen).
 */
#pragma once

#include "mesh_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MeshRoleNone = 0,
    MeshRoleMaster,
    MeshRoleClient,
} MeshRole;

typedef enum {
    MeshEventDiscoverResponse,  // Master: Client hat geantwortet
    MeshEventPairRequest,       // Client: Master will pairen
    MeshEventPairResponse,      // Master: Client hat Pair akzeptiert/abgelehnt
    MeshEventDisconnect,        // Client: Master beendet das Pairing (Auto-ACK
                                //         schickt der Service selbst; UI darf
                                //         master.txt löschen)
} MeshEvent;

typedef struct {
    MeshEvent type;
    uint8_t mac[MESH_MAC_LEN];
    char name[MESH_NAME_MAX + 1];
    bool accepted;  // nur für PairResponse
} MeshEventData;

typedef void (*MeshEventCallback)(const MeshEventData* ev, void* ctx);

/* Lifecycle ─ start/stop sind idempotent. start() blockiert bis WiFi+ESP-NOW
 * online sind (oder Timeout 2 s → false). */
bool mesh_service_start(MeshRole role, MeshEventCallback cb, void* ctx);
void mesh_service_stop(void);
bool mesh_service_is_active(void);
MeshRole mesh_service_get_role(void);

/* Self-MAC (STA-Interface). Liefert nur sinnvolle Werte solange Service aktiv. */
bool mesh_service_get_self_mac(uint8_t out[MESH_MAC_LEN]);

/* Sender ─ async, return true wenn Command gequeued. */
bool mesh_send_discover(void);                                  // master broadcast
bool mesh_send_pair_request(const uint8_t to[MESH_MAC_LEN]);
bool mesh_send_pair_response(const uint8_t to[MESH_MAC_LEN], bool accepted);
bool mesh_send_disconnect(const uint8_t to[MESH_MAC_LEN]);

#ifdef __cplusplus
}
#endif
