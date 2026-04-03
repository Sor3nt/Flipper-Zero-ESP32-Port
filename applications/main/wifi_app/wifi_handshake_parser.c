#include "wifi_handshake_parser.h"
#include <string.h>
#include <esp_log.h>

#define TAG "HsParser"

bool hs_is_beacon(const uint8_t* payload, int len) {
    if(len < 24) return false;
    uint16_t fc = payload[0] | (payload[1] << 8);
    uint8_t frame_type = (fc & 0x0C) >> 2;
    uint8_t frame_subtype = (fc & 0xF0) >> 4;
    return (frame_type == 0 && frame_subtype == 8);
}

bool hs_parse_addresses(
    const uint8_t* payload, int len,
    const uint8_t** bssid, const uint8_t** station, const uint8_t** ap,
    int* header_len) {

    if(len < 24) return false;

    uint16_t fc = payload[0] | (payload[1] << 8);
    uint8_t frame_subtype = (fc & 0xF0) >> 4;
    uint8_t to_ds = (fc & 0x0100) >> 8;
    uint8_t from_ds = (fc & 0x0200) >> 9;

    *header_len = 24;
    if(to_ds && from_ds) *header_len = 30;
    if((frame_subtype & 0x08) == 0x08) *header_len += 2; // QoS

    if(len < *header_len + 8) return false;

    const uint8_t* addr1 = &payload[4];
    const uint8_t* addr2 = &payload[10];
    const uint8_t* addr3 = &payload[16];

    if(!to_ds && from_ds) {
        *bssid = addr2; *ap = addr2; *station = addr1;
    } else if(to_ds && !from_ds) {
        *bssid = addr1; *ap = addr1; *station = addr2;
    } else if(!to_ds && !from_ds) {
        *bssid = addr3; *station = addr2; *ap = addr1;
    } else {
        *bssid = addr1; *station = addr2; *ap = addr1;
    }

    return true;
}

bool hs_is_eapol(const uint8_t* payload, int header_len, int len) {
    if(len < header_len + 8) return false;
    const uint8_t* llc = &payload[header_len];
    return (llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 &&
            llc[3] == 0x00 && llc[4] == 0x00 && llc[5] == 0x00 &&
            llc[6] == 0x88 && llc[7] == 0x8E);
}

uint8_t hs_get_eapol_msg_num(const uint8_t* eapol_start, int eapol_len) {
    if(eapol_len < 4) return 0;
    uint8_t packet_type = eapol_start[1];
    if(packet_type != 0x03) return 0; // 0x03 = EAPOL-Key
    if(eapol_len < 95) return 0;

    uint16_t key_info = (eapol_start[5] << 8) | eapol_start[6];
    bool key_mic  = (key_info & 0x0100) != 0;
    bool secure   = (key_info & 0x0200) != 0;
    bool key_ack  = (key_info & 0x0080) != 0;
    bool install  = (key_info & 0x0040) != 0;
    bool pairwise = (key_info & 0x0008) != 0;

    ESP_LOGD(TAG, "KeyInfo: 0x%04X (MIC=%d SEC=%d ACK=%d INS=%d PWR=%d)",
             key_info, key_mic, secure, key_ack, install, pairwise);

    if(pairwise && key_ack && !install && !key_mic) return 1;
    if(pairwise && !key_ack && !install && key_mic && !secure) return 2;
    if(pairwise && key_ack && install && key_mic) return 3;
    if(pairwise && !key_ack && !install && key_mic && secure) return 4;
    return 0;
}

bool hs_extract_beacon_ssid(const uint8_t* payload, int len, char* ssid_out, int max_len) {
    // Beacon frame: 24B header + 12B fixed params (timestamp+interval+capability)
    // Then tagged parameters starting at offset 36
    if(len < 38) {
        ssid_out[0] = '\0';
        return false;
    }

    int pos = 36; // start of tagged parameters
    while(pos + 2 <= len) {
        uint8_t tag_id = payload[pos];
        uint8_t tag_len = payload[pos + 1];
        if(pos + 2 + tag_len > len) break;

        if(tag_id == 0) { // SSID tag
            int copy_len = tag_len;
            if(copy_len >= max_len) copy_len = max_len - 1;
            memcpy(ssid_out, &payload[pos + 2], copy_len);
            ssid_out[copy_len] = '\0';
            return copy_len > 0;
        }

        pos += 2 + tag_len;
    }

    ssid_out[0] = '\0';
    return false;
}
