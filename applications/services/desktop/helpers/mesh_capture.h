/**
 * Mesh-Capture-Session: empfängt vom Buddy gestreamte 802.11-Frames (über den
 * mesh_service pcap-Sink) und schreibt sie als .pcap auf die SD. Läuft auf
 * Desktop-Ebene (unabhängig von der aktuell aktiven Scene), damit ein laufender
 * Capture auch in der Clients-Liste als aktive Action erscheint.
 *
 * Threading: der Sink wird im WiFi-Task aufgerufen und kopiert Frames nur in
 * einen Ringbuffer; ein eigener Writer-FuriThread schreibt die .pcap (Storage
 * darf nicht aus dem WiFi-Task heraus aufgerufen werden) und treibt zugleich das
 * Stop-Retry (der Buddy ist beim Capturen off-channel und verpasst ein
 * einmaliges Stop — also wiederholen, bis ein Stopped-Status kommt).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mesh_config.h" /* MESH_MAC_LEN */

/* Start einer Capture-Session: öffnet die .pcap, registriert den Sink, startet
 * den Writer-Thread und schickt FeatureStart(feat_id, [channel]) an den Client.
 * channel: 0 = hop (1..13), sonst fixer Kanal. Liefert false bei Fehler. */
bool mesh_capture_start(
    const uint8_t mac[MESH_MAC_LEN],
    const char* client_name,
    uint8_t feat_id,
    uint8_t channel);

/* Wie start, aber OHNE FeatureStart — sammelt die Ergebnisse eines bereits
 * laufenden Buddy-Captures ein (z.B. nach Master-Reboot, sobald der Buddy als
 * "läuft" gemeldet wird). */
bool mesh_capture_attach(
    const uint8_t mac[MESH_MAC_LEN],
    const char* client_name,
    uint8_t feat_id,
    uint8_t channel);

/* Beginnt das Stoppen: Status → Stopping, Writer-Thread schickt FeatureStop
 * wiederholt bis Stopped/Timeout, dann wird finalisiert. Nicht-blockierend. */
void mesh_capture_stop(void);

/* Hart beenden (z.B. bei Disconnect/Verlassen): Writer-Thread joinen, Datei zu. */
void mesh_capture_finish(void);

bool mesh_capture_is_active(void);
bool mesh_capture_get_mac(uint8_t out[MESH_MAC_LEN]);
uint32_t mesh_capture_frame_count(void);
const char* mesh_capture_path(void); /* "" wenn inaktiv */

/* Vom Desktop-Custom-Event-Handler bei einem FeatureStatus aufrufen — beendet
 * das Stop-Retry, sobald der passende Client Stopped meldet. */
void mesh_capture_note_status(const uint8_t mac[MESH_MAC_LEN], uint8_t feat_id, uint8_t state);
