/**
 * Mesh-Capture (Master): schreibt einen vom Buddy zuverlässig gelieferten
 * WPA-Handshake als .pcap auf die SD — eine Datei pro Netz:
 *   /ext/wifi/buddy_<client>_<ssid>.pcap   (CREATE_ALWAYS, also überschrieben).
 *
 * Der Buddy hält die EAPOL-Frames (M1..M4) + Beacon pro BSSID durabel und liefert
 * jeden vollständigen Handshake als EINE fragmentierte, quittierte Result-Einheit
 * (mesh_service reassembliert sie). `blob` ist genau dieses Payload:
 *   [type:1][ssid_len:1][ssid][frame_count:1]{ [msg_num:1][len:2 LE][bytes] }
 *
 * Wird im GUI-Thread aufgerufen (Storage-Zugriff dort erlaubt — NICHT aus dem
 * WiFi-/ESP-NOW-Kontext).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Schreibt den Handshake-Blob als .pcap. Liefert false bei Parse-/IO-Fehler. */
bool mesh_capture_write_handshake(const char* client_name, const uint8_t* blob, uint16_t blob_len);
