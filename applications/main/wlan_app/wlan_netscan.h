#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t ip;       // Network-Byte-Order
    uint8_t mac[6];
    char hostname[24]; // leer wenn (noch) nicht aufgelöst
} WlanNetscanHost;

/** Reset internal scanner state. Call when entering the scanning scene. */
void wlan_netscan_reset(void);

/** Pro Tick aufrufen, sendet ein paar etharp_request für IPs im /24 und
 *  liest die ARP-Tabelle aus. Liefert true wenn alle IPs gescannt sind. */
bool wlan_netscan_arp_step(void);

/** Anzahl bisher gefundener Hosts. */
uint8_t wlan_netscan_get_host_count(void);

/** Gibt einen Snapshot des Hosts an Index zurück. */
bool wlan_netscan_get_host(uint8_t idx, WlanNetscanHost* out);

/** Aktueller Scan-Fortschritt in Prozent (0..100). */
uint8_t wlan_netscan_get_progress(void);

/** Startet asynchronen Hostname-Resolve-Worker (xTaskCreate). Iteriert durch
 *  alle bisher gefundenen Hosts und versucht NetBIOS-Lookup. Schreibt
 *  Ergebnis in s_hosts[i].hostname. Idempotent. */
void wlan_netscan_start_hostname_resolve(void);

/** true wenn der Hostname-Worker fertig ist. */
bool wlan_netscan_hostname_done(void);

/** Stoppt einen ggf. laufenden Hostname-Worker. */
void wlan_netscan_stop_hostname_resolve(void);
