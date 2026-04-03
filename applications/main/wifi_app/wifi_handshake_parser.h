#pragma once

#include <stdint.h>
#include <stdbool.h>

/** Check if frame is a beacon (management type 0, subtype 8) */
bool hs_is_beacon(const uint8_t* payload, int len);

/** Parse 802.11 addresses based on toDS/fromDS flags.
 *  Returns false if frame is too short. Sets header_len to actual 802.11 header size. */
bool hs_parse_addresses(
    const uint8_t* payload, int len,
    const uint8_t** bssid, const uint8_t** station, const uint8_t** ap,
    int* header_len);

/** Check LLC/SNAP header for EAPOL EtherType (0x888E) */
bool hs_is_eapol(const uint8_t* payload, int header_len, int len);

/** Determine EAPOL-Key message number from KeyInfo bitfield.
 *  eapol_start points to the EAPOL header (after LLC/SNAP).
 *  Returns 1-4 or 0 if not a valid EAPOL-Key. */
uint8_t hs_get_eapol_msg_num(const uint8_t* eapol_start, int eapol_len);

/** Extract SSID from a beacon frame payload.
 *  Writes up to max_len-1 chars + null terminator.
 *  Returns true if SSID was found. */
bool hs_extract_beacon_ssid(const uint8_t* payload, int len, char* ssid_out, int max_len);
