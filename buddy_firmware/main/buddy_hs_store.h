#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "buddy_protocol.h"

/* Durabler Handshake-Store: pro BSSID werden der Beacon (für SSID-Kontext) und
 * die EAPOL-Frames M1..M4 festgehalten. Ein Record gilt als „vollständig", sobald
 * M2 & M3 da sind. Vollständige, noch nicht vom Master quittierte Records werden
 * in NVS persistiert (überstehen Buddy-Reboot/Stromverlust). Geliefert wird jeder
 * vollständige Handshake als EINE fragmentierte, quittierte Result-Einheit
 * (BuddyWireResult); der Master baut daraus die .pcap und ackt — erst dann wird
 * der Record gelöscht. So überlebt ein Capture beliebig lange Master-Abwesenheit.
 *
 * Thread-safe (interner Mutex): rx/parse + pump laufen im Capture-Task, ack +
 * pump auch im Buddy-Worker-Task. */

#define HS_MAX_RECORDS 4
#define HS_FRAME_MAX   256 /* cap je 802.11-Frame (EAPOL/Beacon passen) */
#define HS_SLOTS       5 /* 0 = Beacon, 1..4 = EAPOL M1..M4 */

void buddy_hs_store_init(void);

/* Beacon eines BSSID festhalten (für SSID + pcap-Kontext). */
void buddy_hs_store_beacon(
    const uint8_t bssid[6],
    const char* ssid,
    const uint8_t* frame,
    uint16_t len);

/* EAPOL-Frame Mn (msg_num 1..4) festhalten. ssid darf "" sein (kommt sonst vom
 * Beacon). Liefert true, wenn der Record dadurch GERADE vollständig wurde. */
bool buddy_hs_store_eapol(
    const uint8_t bssid[6],
    const char* ssid,
    uint8_t msg_num,
    const uint8_t* frame,
    uint16_t len);

/* Alle vollständigen, noch nicht quittierten Records an den Master senden. */
void buddy_hs_store_pump(const uint8_t master_mac[6]);

/* Master hat das Result mit dieser id gespeichert → Record löschen (+ NVS). */
void buddy_hs_store_ack(uint8_t id);

bool buddy_hs_store_has_pending(void);

/* Anzahl vollständiger Records (für die Live-Status-Anzeige). */
uint8_t buddy_hs_store_complete_count(void);
