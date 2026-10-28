/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BW16_PCAP_H
#define BW16_PCAP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define BWC_MAX_FRAME 1600U
#define BWC_LINE_SIZE 3264U

typedef enum { BwcIgnore = 0, BwcBeacon = 1, BwcEapol = 2 } BwcKind;
typedef struct {
    uint32_t session, sequence, usec;
    uint16_t length;
    uint8_t kind;
    uint8_t data[BWC_MAX_FRAME];
} BwcPacket;

/* Raw 802.11 bytes, with no radio header. Unsupported/malformed frames are ignored. */
BwcKind bwc_classify(const uint8_t* frame, size_t length, const uint8_t bssid[6]);
/* PCAP1:<session8>:<sequence8>:<usec8>:<length4>:<kind2>:<crc8>:<hex>.
 * All numeric wire fields are hexadecimal. CRC32/IEEE covers little-endian
 * session, sequence, usec, length, then kind and the exact packet bytes.
 * Encode adds NUL, not LF, and returns the character count excluding NUL.
 * Decode accepts an exact NUL-terminated line, without CR/LF, and does not
 * change the destination on failure. Neither function allocates memory. */
size_t bwc_encode(char* out, size_t capacity, const BwcPacket* packet);
bool bwc_decode(const char* line, BwcPacket* packet);
/* Classic little-endian PCAP, microsecond timestamps, snaplen 1600, linktype 105.
 * Received frames are kept intact; the linktype does not claim an FCS length. */
void bwc_pcap_header(uint8_t out[24]);
bool bwc_pcap_record_header(
    uint8_t out[16], uint32_t seconds, uint32_t microseconds, uint32_t length);

#ifdef __cplusplus
}
#endif
#endif
