#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Download von sdcard.zip + Entpacken auf /ext. Läuft in einem dedizierten
 *  xTaskCreate-Task (esp_http_client nutzt lwIP-Sockets, die nicht aus
 *  FuriThreads getrieben werden dürfen). Fortschritt wird über volatile
 *  Felder publiziert und von der Scene im Tick gepollt. */

typedef enum {
    WlanSdUpdateIdle = 0,
    WlanSdUpdateChecking,
    WlanSdUpdateDownloading,
    WlanSdUpdateExtracting,
    WlanSdUpdateDone,
    WlanSdUpdateUpToDate,
    WlanSdUpdateError,
} WlanSdUpdatePhase;

typedef struct WlanSdUpdate WlanSdUpdate;

WlanSdUpdate* wlan_sd_update_alloc(void);
void wlan_sd_update_free(WlanSdUpdate* u);

/** Startet den Worker. Kein Effekt wenn bereits aktiv. */
void wlan_sd_update_start(WlanSdUpdate* u);

/** Fordert Abbruch an und blockt (bis ~5 s) bis der Worker beendet ist. */
void wlan_sd_update_cancel(WlanSdUpdate* u);

WlanSdUpdatePhase wlan_sd_update_get_phase(const WlanSdUpdate* u);
uint8_t wlan_sd_update_get_percent(const WlanSdUpdate* u);
const char* wlan_sd_update_get_error(const WlanSdUpdate* u);
bool wlan_sd_update_is_running(const WlanSdUpdate* u);

/** Aktuell bearbeitete Datei (Anzeige). Beim Start "version.txt". */
const char* wlan_sd_update_get_current_file(const WlanSdUpdate* u);

/** Aktuelle Download-Geschwindigkeit in kB/s (0 wenn nicht ladend). */
uint32_t wlan_sd_update_get_speed_kbps(const WlanSdUpdate* u);

/** Bearbeitete bzw. Gesamtanzahl Manifest-Einträge (für "<n>/<max>"). */
uint32_t wlan_sd_update_get_done(const WlanSdUpdate* u);
uint32_t wlan_sd_update_get_total(const WlanSdUpdate* u);
