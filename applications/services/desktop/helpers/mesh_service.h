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
    MeshEventFeatureList,       // Master: Client hat seine Feature-Liste geschickt
    MeshEventFeatureStatus,     // Master: Client meldet Feature-Status (run/stop/data)
    MeshEventResult,            // Master: Client liefert ein Resultat (zuverlässig, ack)
} MeshEvent;

/* Feature-Status-Werte (müssen zum Buddy buddy_protocol.h BuddyFeatState passen). */
typedef enum {
    MeshFeatStateStopped = 0,
    MeshFeatStateRunning = 1,
    MeshFeatStateError   = 2,
    MeshFeatStateData    = 3,
} MeshFeatState;

#define MESH_FEAT_NAME_MAX 20
#define MESH_FEATURES_MAX  8
#define MESH_FEAT_DATA_MAX 40

/* Result-Typen (müssen zum Buddy buddy_protocol.h BuddyResultType passen). */
typedef enum {
    MeshResultHandshake = 1, /* feat_data = SSID-String */
} MeshResultType;

typedef struct {
    uint8_t id;
    char name[MESH_FEAT_NAME_MAX + 1];
} MeshFeature;

typedef struct {
    MeshEvent type;
    uint8_t mac[MESH_MAC_LEN];
    char name[MESH_NAME_MAX + 1];
    bool accepted;       // nur für PairResponse
    uint32_t caps;       // DiscoverResponse / PairResponse: Feature-Bitmask des Clients
    uint8_t rx_channel;  // WiFi-Kanal, auf dem dieses Paket ankam (= Kanal des Buddys)

    // MeshEventFeatureList
    MeshFeature features[MESH_FEATURES_MAX];
    uint8_t feature_count;
    uint32_t running_mask;  // welche Feature-IDs gerade laufen (Quelle: Buddy)

    // MeshEventFeatureStatus
    uint8_t feat_id;
    uint8_t feat_state;  // MeshFeatState
    uint8_t feat_data[MESH_FEAT_DATA_MAX + 1]; // +1: NUL-terminierbar für Text-Display
    uint8_t feat_data_len;

    // MeshEventResult — Nutzdaten liegen ebenfalls in feat_data/feat_data_len
    // (NUL-terminiert), z.B. die SSID bei einem Handshake-Resultat.
    uint8_t result_id;    // Buddy-lokale id (für ResultAck)
    uint8_t result_type;  // BuddyResultType (1 = Handshake)
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

/* Radio-Kanal des Master-Service umstellen (1..13). Async über die Worker-Queue;
 * ein kurz davor gesendetes Paket geht noch auf dem alten Kanal raus. Wird beim
 * Capture benutzt, um dem Buddy auf seinen Capture-Kanal zu folgen, und für den
 * Discovery-Channel-Sweep. */
void mesh_service_set_channel(uint8_t channel);

/* Sender ─ async, return true wenn Command gequeued. */
bool mesh_send_discover(void);                                  // master broadcast
bool mesh_send_pair_request(const uint8_t to[MESH_MAC_LEN]);
bool mesh_send_pair_response(const uint8_t to[MESH_MAC_LEN], bool accepted);
bool mesh_send_disconnect(const uint8_t to[MESH_MAC_LEN]);

/* Master → Client Feature-Steuerung. */
bool mesh_send_feature_query(const uint8_t to[MESH_MAC_LEN]);   // -> MeshEventFeatureList
bool mesh_send_feature_start(
    const uint8_t to[MESH_MAC_LEN],
    uint8_t feat_id,
    const uint8_t* args,
    uint8_t arg_len);
bool mesh_send_feature_stop(const uint8_t to[MESH_MAC_LEN], uint8_t feat_id);

/* Master → Client: bestätigt ein empfangenes Resultat (Buddy löscht es dann). */
bool mesh_send_result_ack(const uint8_t to[MESH_MAC_LEN], uint8_t result_id);

/* Pcap-Sink: empfängt vom Client gestreamte, reassemblierte rohe 802.11-Frames
 * (BuddyWirePcapFrame). Wird im WiFi-Task-Kontext aufgerufen — der Sink muss
 * schnell sein (z.B. in einen Ringbuffer kopieren, ein Writer-Thread schreibt
 * die .pcap). sink==NULL deregistriert. */
typedef void (*MeshPcapSink)(const uint8_t* frame, uint16_t len, void* ctx);
void mesh_set_pcap_sink(MeshPcapSink sink, void* ctx);

#ifdef __cplusplus
}
#endif
