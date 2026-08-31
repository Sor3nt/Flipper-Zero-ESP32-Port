#pragma once

#include <stdbool.h>
#include "wifi_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RECORD_WIFI "wifi"

typedef struct Wifi Wifi;

/** Ist WiFi global aktiviert (Radio soll an sein, BLE aus)? Lock-free Read. */
bool wifi_is_enabled(Wifi* wifi);

/** Ist die STA aktuell verbunden (hat eine IP)? */
bool wifi_is_connected(Wifi* wifi);

/** Kopiert die aktuellen Settings nach out. */
void wifi_get_settings(Wifi* wifi, WifiSettings* out);

/** WiFi global einschalten: BLE-Stack abschalten (RAM frei) + persistieren,
 *  Radio hoch, und — falls ein last_ssid gemerkt ist — automatisch reconnecten.
 *  Idempotent (wenn schon an, nur Reconnect-Versuch). */
void wifi_enable(Wifi* wifi);

/** WiFi global ausschalten: Radio runter, enabled=false persistieren. BLE wird
 *  NICHT automatisch reaktiviert (dafür Lock-Menü „Enable Bluetooth"). */
void wifi_disable(Wifi* wifi);

/** Nach einem erfolgreichen STA-Connect zu rufen: markiert WiFi als global/
 *  „sticky" (enabled=true, BLE aus + persistiert) und merkt sich die SSID für
 *  Auto-Reconnect. Dadurch bleibt die Verbindung nach App-Verlassen bestehen. */
void wifi_mark_connected(Wifi* wifi, const char* ssid);

#ifdef __cplusplus
}
#endif
