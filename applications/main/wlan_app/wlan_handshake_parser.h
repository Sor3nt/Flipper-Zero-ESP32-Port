#pragma once

#include <stdint.h>
#include <stdbool.h>

/** Beacon-Frame? (Mgmt type 0, subtype 8) */
bool wlan_hs_is_beacon(const uint8_t* payload, int len);

/** 802.11-Adressen je nach toDS/fromDS-Flags. Setzt header_len. */
bool wlan_hs_parse_addresses(
    const uint8_t* payload, int len,
    const uint8_t** bssid, const uint8_t** station, const uint8_t** ap,
    int* header_len);

/** LLC/SNAP mit EAPOL-EtherType (0x888E)? */
bool wlan_hs_is_eapol(const uint8_t* payload, int header_len, int len);

/** EAPOL-Key Message-Nummer (1..4) anhand KeyInfo-Bitfield. 0 wenn ungültig. */
uint8_t wlan_hs_get_eapol_msg_num(const uint8_t* eapol_start, int eapol_len);

/** SSID aus Beacon-Tagged-Parameters extrahieren. */
bool wlan_hs_extract_beacon_ssid(const uint8_t* payload, int len, char* ssid_out, int max_len);
