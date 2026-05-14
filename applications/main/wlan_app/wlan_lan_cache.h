#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "wlan_app.h"

/** Prüft ob /ext/wifi/lan/<ssid>.csv existiert. */
bool wlan_lan_cache_exists(const char* ssid);

/** Lädt gecachte Devices für SSID aus /ext/wifi/lan/<ssid>.csv.
 *  Füllt out_devices[0..*out_count-1] mit ip/mac/hostname. Andere Felder
 *  (active, block_internet, throttle_kbps, sniff_monitor) werden auf 0
 *  initialisiert. Liefert true wenn ≥ 1 Device geladen wurde. */
bool wlan_lan_cache_load(
    const char* ssid,
    WlanDeviceRecord* out_devices,
    uint16_t max,
    uint16_t* out_count);

/** Schreibt/überschreibt /ext/wifi/lan/<ssid>.csv mit count Devices.
 *  Persistiert nur ip/mac/hostname — Operations-Flags werden bewusst
 *  nicht serialisiert. */
bool wlan_lan_cache_save(
    const char* ssid,
    const WlanDeviceRecord* devices,
    uint16_t count);
