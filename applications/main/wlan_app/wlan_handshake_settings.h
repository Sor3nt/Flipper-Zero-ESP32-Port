#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WLAN_HS_SETTINGS_PATH     "/ext/wifi/handshake.cfg"
#define WLAN_HS_SETTINGS_MAGIC    0x42
#define WLAN_HS_SETTINGS_VERSION  1
#define WLAN_HS_SAVE_PATH_MAX     96
#define WLAN_HS_DEFAULT_SAVE_PATH "/ext/wifi/handshakes"

typedef struct {
    uint8_t channel;  // 1..13 (Hopping ignoriert das, sonst fester Channel)
    bool hopping;     // true → Auto-Rotate durch alle 13 Channels
    char save_path[WLAN_HS_SAVE_PATH_MAX]; // Ordner für PCAP-Files
} WlanHandshakeSettings;

/** Lädt Settings aus /ext/wifi/handshake.cfg; bei Miss/Fail werden Defaults
 *  geschrieben (channel=1, hopping=false, save_path="/ext/wifi/handshakes"). */
void wlan_handshake_settings_load(WlanHandshakeSettings* out);

/** Persistiert die Settings via saved_struct. */
void wlan_handshake_settings_save(const WlanHandshakeSettings* in);
