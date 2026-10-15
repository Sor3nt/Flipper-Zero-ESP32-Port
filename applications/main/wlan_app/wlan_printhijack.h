#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Printer-Hijack ("Weg A"): findet _ipp._tcp-Drucker im LAN und übernimmt einen
 * auf Discovery-Ebene. Der Flipper BEHÄLT seine eigene IP, bewirbt den
 * _ipp._tcp-Dienst aber unter dem NAMEN des Zieldruckers (mDNS-Clone, SRV/A →
 * dieses Gerät); der IPP-Server (wlan_airprint) nimmt die Jobs entgegen.
 * Parallel wird der echte Drucker gezielt per Deauth vom WLAN geworfen (nur
 * dessen MAC, auf dem aktuellen Kanal, OHNE die eigene STA zu trennen), damit er
 * seinen mDNS-Namen nicht mehr verteidigt und die Clients auf uns auflösen.
 *
 * Weg B (IP-Übernahme + ARP) wurde verworfen: auf dem ESP/WLAN führte er nur zu
 * einem DoS (Drucker offline), weil macOS gratuitous ARP für gecachte Einträge
 * ignoriert und der IP-Konflikt über den AP beide Seiten unerreichbar macht. */

#define WLAN_PRINTHIJACK_NAME_MAX 41
#define WLAN_PRINTHIJACK_MAX      16

typedef struct {
    char name[WLAN_PRINTHIJACK_NAME_MAX];
    uint32_t ip; /* network byte order */
    uint16_t port;
    uint8_t mac[6]; /* WiFi-MAC (via ARP), 0 wenn nicht auflösbar → kein Deauth */
    bool has_mac;
} WlanPrinterEntry;

/* mDNS-Scan nach _ipp._tcp-Druckern. Füllt out (bis max), liefert die Anzahl.
 * Blockiert ~timeout_ms (läuft intern über den wlan_hal-Worker). */
int wlan_printhijack_scan(WlanPrinterEntry* out, int max, uint32_t timeout_ms);

/* Startet den Hijack auf das gewählte Ziel: mDNS-Clone des Namens auf dieses
 * Gerät + (falls MAC bekannt) gezielter Deauth des echten Druckers. */
bool wlan_printhijack_start(uint32_t ip, const char* name, const uint8_t* mac, bool has_mac);

/* Stoppt Hijack: Deauth aus, Clone/Server aus. */
void wlan_printhijack_stop(void);

bool wlan_printhijack_is_running(void);

/* Eigene IP (dorthin landen die Jobs) als String (len >= 16). */
bool wlan_printhijack_get_ip(char* out, size_t len);
