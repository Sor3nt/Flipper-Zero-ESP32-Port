#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Global (über App-Grenzen persistenter) WiFi-Zustand — analog zu BtSettings.
 *  `enabled`  : WiFi soll global an sein (Radio an, BLE aus). Gesetzt via
 *               Lock-Menü „Enable WiFi" oder nach einem erfolgreichen
 *               STA-Connect; überlebt App-Wechsel und Reboot.
 *  `last_ssid`: zuletzt erfolgreich verbundenes Netz — beim Enable/Boot wird
 *               (mit dem auf der SD gespeicherten Passwort) automatisch
 *               reconnectet. Leerer String = kein Auto-Reconnect. */
typedef struct {
    bool enabled;
    char last_ssid[33];
} WifiSettings;

void wifi_settings_load(WifiSettings* settings);

void wifi_settings_save(const WifiSettings* settings);

#ifdef __cplusplus
}
#endif
