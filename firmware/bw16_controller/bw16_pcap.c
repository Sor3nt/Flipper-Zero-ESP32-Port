/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bw16_pcap.h"
#include <string.h>

BwcKind bwc_classify(const uint8_t* frame, size_t length, const uint8_t bssid[6]) {
    if(!frame || !bssid || length < 24U || length > BWC_MAX_FRAME ||
       (frame[0] & 3U) || (frame[1] & 0x44U) || (frame[22] & 15U)) return BwcIgnore;
    const unsigned ds = frame[1] & 3U;
    if(frame[0] == 0x80U) {
        return !ds && length >= 36U && !memcmp(frame + 16, bssid, 6) ?
                   BwcBeacon : BwcIgnore;
    }
    /* Data-bearing subtypes are 0..3 and 8..11. No WDS BSSID is implied. */
    if((frame[0] & 0x0cU) != 8U || (frame[0] & 0x40U) || ds == 3U)
        return BwcIgnore;
    const size_t bssid_offset = ds == 1U ? 4U : ds == 2U ? 10U : 16U;
    if(memcmp(frame + bssid_offset, bssid, 6)) return BwcIgnore;
    size_t header = 24U;
    if(frame[0] & 0x80U) {
        if(length < header + 2U || (frame[header] & 0x80U)) return BwcIgnore;
        /* Mesh control uses another payload layout; do not misread it as LLC. */
        if(!ds && (frame[header + 1U] & 1U)) return BwcIgnore;
        header += 2U;
        if(frame[1] & 0x80U) header += 4U;
    }
    static const uint8_t snap[8] = {0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e};
    if(length < header + sizeof(snap) + 4U || memcmp(frame + header, snap, sizeof(snap)))
        return BwcIgnore;
    const uint8_t* eapol = frame + header + sizeof(snap);
    const size_t body_length = ((size_t)eapol[2] << 8) | eapol[3];
    /* Preserve every EAPOL type, including EAP/Start/Logoff, without claiming
     * that an EAPOL-Key packet or any count proves a complete handshake. */
    if(eapol[0] == 0 || body_length > length - header - sizeof(snap) - 4U)
        return BwcIgnore;
    return BwcEapol;
}

static uint32_t crc_byte(uint32_t crc, uint8_t byte) {
    crc ^= byte;
    for(unsigned i = 0; i < 8; ++i)
        crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    return crc;
}
static uint32_t crc_integer(uint32_t crc, uint32_t value, unsigned bytes) {
    for(unsigned i = 0; i < bytes; ++i) {
        crc = crc_byte(crc, (uint8_t)value);
        value >>= 8;
    }
    return crc;
}
static uint32_t crc_metadata(
    uint32_t session, uint32_t sequence, uint32_t usec, uint16_t length, uint8_t kind) {
    uint32_t crc = crc_integer(UINT32_MAX, session, 4);
    crc = crc_integer(crc, sequence, 4);
    crc = crc_integer(crc, usec, 4);
    crc = crc_integer(crc, length, 2);
    return crc_byte(crc, kind);
}
static void hex_integer(char* out, uint32_t value, unsigned digits) {
    static const char hex[] = "0123456789ABCDEF";
    for(unsigned i = 0; i < digits; ++i) {
        out[digits - i - 1U] = hex[value & 15U];
        value >>= 4;
    }
}
static int unhex(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool parse_hex(const char* text, unsigned digits, uint32_t* value) {
    uint32_t result = 0;
    for(unsigned i = 0; i < digits; ++i) {
        int digit = unhex(text[i]);
        if(digit < 0) return false;
        result = (result << 4) | (unsigned)digit;
    }
    *value = result;
    return true;
}
static bool valid_packet(uint32_t length, uint32_t kind) {
    return length > 0 && length <= BWC_MAX_FRAME &&
           (kind == BwcBeacon || kind == BwcEapol);
}
size_t bwc_encode(char* out, size_t capacity, const BwcPacket* packet) {
    if(!out || !packet || !valid_packet(packet->length, packet->kind)) return 0;
    const size_t count = 50U + 2U * packet->length;
    if(capacity <= count) return 0;
    uint32_t crc = crc_metadata(
        packet->session, packet->sequence, packet->usec, packet->length, packet->kind);
    for(size_t i = 0; i < packet->length; ++i) crc = crc_byte(crc, packet->data[i]);
    memcpy(out, "PCAP1:", 6);
    hex_integer(out + 6, packet->session, 8); out[14] = ':';
    hex_integer(out + 15, packet->sequence, 8); out[23] = ':';
    hex_integer(out + 24, packet->usec, 8); out[32] = ':';
    hex_integer(out + 33, packet->length, 4); out[37] = ':';
    hex_integer(out + 38, packet->kind, 2); out[40] = ':';
    hex_integer(out + 41, crc ^ UINT32_MAX, 8); out[49] = ':';
    for(size_t i = 0; i < packet->length; ++i) hex_integer(out + 50U + 2U * i, packet->data[i], 2);
    out[count] = 0;
    return count;
}
bool bwc_decode(const char* line, BwcPacket* packet) {
    if(!line || !packet) return false;
    size_t count = 0;
    while(count < BWC_LINE_SIZE && line[count]) ++count;
    if(count < 52U || count >= BWC_LINE_SIZE || memcmp(line, "PCAP1:", 6) ||
       line[14] != ':' || line[23] != ':' || line[32] != ':' || line[37] != ':' ||
       line[40] != ':' || line[49] != ':') return false;
    uint32_t session, sequence, usec, length, kind, expected;
    if(!parse_hex(line + 6, 8, &session) || !parse_hex(line + 15, 8, &sequence) ||
       !parse_hex(line + 24, 8, &usec) || !parse_hex(line + 33, 4, &length) ||
       !parse_hex(line + 38, 2, &kind) || !parse_hex(line + 41, 8, &expected) ||
       !valid_packet(length, kind) || count != 50U + 2U * length) return false;
    uint32_t crc = crc_metadata(session, sequence, usec, (uint16_t)length, (uint8_t)kind);
    for(size_t i = 0; i < length; ++i) {
        uint32_t byte;
        if(!parse_hex(line + 50U + 2U * i, 2, &byte)) return false;
        crc = crc_byte(crc, (uint8_t)byte);
    }
    if((crc ^ UINT32_MAX) != expected) return false;
    packet->session = session; packet->sequence = sequence; packet->usec = usec;
    packet->length = (uint16_t)length; packet->kind = (uint8_t)kind;
    for(size_t i = 0; i < length; ++i) {
        const char* hex = line + 50U + 2U * i;
        packet->data[i] = (uint8_t)((unhex(hex[0]) << 4) | unhex(hex[1]));
    }
    return true;
}

static void little32(uint8_t* out, uint32_t value) {
    for(unsigned i = 0; i < 4; ++i) { out[i] = (uint8_t)value; value >>= 8; }
}
void bwc_pcap_header(uint8_t out[24]) {
    if(!out) return;
    memset(out, 0, 24);
    little32(out, 0xa1b2c3d4U);
    out[4] = 2; out[6] = 4;
    little32(out + 16, BWC_MAX_FRAME);
    little32(out + 20, 105);
}
bool bwc_pcap_record_header(
    uint8_t out[16], uint32_t seconds, uint32_t microseconds, uint32_t length) {
    if(!out || !length || length > BWC_MAX_FRAME || microseconds >= 1000000U) return false;
    little32(out, seconds); little32(out + 4, microseconds);
    little32(out + 8, length); little32(out + 12, length);
    return true;
}
