#pragma once

/* 802.11 handshake parsing — ported verbatim from the T-Embed firmware
 * (applications/main/wlan_app/wlan_handshake_parser.{c,h}). Pure logic, no
 * platform deps, so the buddy uses the exact same EAPOL/beacon detection as the
 * master's wlan_app. */

#include <stdbool.h>
#include <stdint.h>

/** Beacon frame? (Mgmt type 0, subtype 8) */
bool wlan_hs_is_beacon(const uint8_t* payload, int len);

/** 802.11 addresses by toDS/fromDS flags. Sets header_len. */
bool wlan_hs_parse_addresses(
    const uint8_t* payload,
    int len,
    const uint8_t** bssid,
    const uint8_t** station,
    const uint8_t** ap,
    int* header_len);

/** LLC/SNAP with EAPOL ethertype (0x888E)? */
bool wlan_hs_is_eapol(const uint8_t* payload, int header_len, int len);

/** EAPOL-Key message number (1..4) from the KeyInfo bitfield. 0 if invalid. */
uint8_t wlan_hs_get_eapol_msg_num(const uint8_t* eapol_start, int eapol_len);

/** Extract SSID from a beacon's tagged parameters. */
bool wlan_hs_extract_beacon_ssid(const uint8_t* payload, int len, char* ssid_out, int max_len);
