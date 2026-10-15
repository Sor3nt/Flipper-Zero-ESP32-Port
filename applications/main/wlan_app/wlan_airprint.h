#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* AirPrint / IPP-Everywhere: bewirbt den Flipper per mDNS als Netzwerkdrucker
 * (_ipp._tcp mit AirPrint-TXT-Records) und nimmt über einen kleinen IPP-Server
 * (raw-TCP, minimales HTTP/1.1) Druckjobs von iPhone/iPad/Mac/Android entgegen.
 * Eingehende Dokumente werden formaterhaltend nach /ext/wifi/printer/ gestreamt
 * (PDF direkt, JPEG/PNG/PS/URF/PWG roh — keine Konvertierung auf dem ESP).
 *
 * Portierung des Desktop-Tools /Users/matthias/www/privat/fakePrinter.
 *
 * Läuft nur über eine bestehende STA-Verbindung (wlan_hal_is_connected()). Der
 * IPP-Server ist ein eigener FreeRTOS-Task (lwIP-Sockets → xTaskCreate, nicht
 * FuriThread); die mDNS-Registrierung läuft über den wlan_hal-Worker. */

#define WLAN_AIRPRINT_PORT   631 /* Standard-IPP-Port; auf dem ESP frei bindbar */
#define WLAN_AIRPRINT_DIR    "/ext/wifi/printer"
#define WLAN_AIRPRINT_NAME_MAX 40 /* max. Länge des Anzeigenamens (ohne Nullbyte) */
#define WLAN_AIRPRINT_SPAM_MAX 30 /* max. Anzahl gespammter Druckernamen */

/* Startet mDNS-Advertising + IPP-Server auf der aktuellen STA-Verbindung.
 * Blockiert bis der Server lauscht (oder ein Fehler auftrat). */
bool wlan_airprint_start(void);

/* Wie wlan_airprint_start(), bewirbt aber viele Druckernamen aus
 * /ext/wifi/printer/spam.txt (ein Name pro Zeile, #-Kommentare/Leerzeilen
 * ignoriert), die alle auf dieses Gerät zeigen. Ein gemeinsamer IPP-Server
 * nimmt die Jobs entgegen. false wenn keine Namen gelesen wurden. */
bool wlan_airprint_start_spam(void);

/* Anzahl aktuell gespammter Druckernamen (0 im Normalbetrieb). */
uint16_t wlan_airprint_get_spam_count(void);

/* Startet NUR den IPP-Server (Port 631), OHNE mDNS und OHNE die eigene IP zu
 * ändern. Für den Printer-Hijack (wlan_printhijack): dort übernimmt das Modul
 * die IP des Zieldruckers, und dieser Server nimmt die umgeleiteten Jobs an. */
bool wlan_airprint_start_capture(void);

/* Startet mDNS + IPP-Server, bewirbt aber den Dienst unter dem übergebenen
 * Instanznamen (Printer-Hijack „Clone": Name des Zieldruckers), OHNE die
 * persistierte config.txt zu ändern. SRV/A zeigen auf dieses Gerät (eigene IP
 * bleibt). */
bool wlan_airprint_start_clone(const char* name);

/* Stoppt Server + mDNS. Wartet auf das Ende des Server-Tasks. Idempotent. */
void wlan_airprint_stop(void);

bool wlan_airprint_is_running(void);

/* Schreibt die Server-IP (STA) in out (len >= 16). */
bool wlan_airprint_get_ip(char* out, size_t len);

uint16_t wlan_airprint_get_port(void);

/* Angezeigter Druckername (wie in der Druckerauswahl des Handys). Wird beim
 * ersten Aufruf aus /ext/wifi/printer/config.txt geladen (sonst Default). */
const char* wlan_airprint_get_name(void);

/* Setzt den Anzeigenamen und persistiert ihn nach config.txt. Wirkt beim
 * nächsten Start des Servers (mDNS wird mit dem neuen Namen registriert). */
void wlan_airprint_set_name(const char* name);

/* Anzahl bisher empfangener/gespeicherter Dokumente in dieser Session. */
uint32_t wlan_airprint_get_job_count(void);

/* Basename der zuletzt gespeicherten Datei ("" wenn noch keine). */
const char* wlan_airprint_get_last_file(void);
