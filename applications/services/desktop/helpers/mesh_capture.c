#include "mesh_capture.h"

#include <furi.h>
#include <storage/storage.h>
#include <string.h>

#define TAG "MeshCapture"

#define CAP_DIR     "/ext/wifi"
#define CAP_SNAPLEN 512

/* pcap (libpcap classic), DLT 105 = LINKTYPE_IEEE802_11. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t network;
} PcapGlobalHeader;

typedef struct __attribute__((packed)) {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
} PcapPacketHeader;

static void sanitize(const char* in, char* out, size_t out_len) {
    size_t j = 0;
    for(size_t i = 0; in && in[i] && j < out_len - 1; ++i) {
        char c = in[i];
        if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    if(j == 0) out[j++] = 'x';
    out[j] = '\0';
}

bool mesh_capture_write_handshake(const char* client_name, const uint8_t* blob, uint16_t blob_len) {
    if(!blob || blob_len < 3) return false;

    /* Header parsen: [type][ssid_len][ssid][frame_count] */
    size_t o = 0;
    o++; /* type (nur Handshake derzeit) */
    uint8_t ssid_len = blob[o++];
    if(ssid_len > 32 || o + ssid_len + 1 > blob_len) return false;
    char ssid[33];
    memcpy(ssid, &blob[o], ssid_len);
    ssid[ssid_len] = '\0';
    o += ssid_len;
    uint8_t frame_count = blob[o++];
    if(frame_count == 0) return false;

    /* Dateiname: /ext/wifi/buddy_<client>_<ssid>.pcap (pro Netz, überschrieben). */
    char safe_name[40], safe_ssid[40];
    sanitize(client_name, safe_name, sizeof(safe_name));
    sanitize(ssid[0] ? ssid : "unknown", safe_ssid, sizeof(safe_ssid));

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, CAP_DIR);

    char path[200];
    snprintf(path, sizeof(path), "%s/buddy_%s_%s.pcap", CAP_DIR, safe_name, safe_ssid);

    File* f = storage_file_alloc(storage);
    if(!storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FURI_LOG_E(TAG, "pcap open failed: %s", path);
        storage_file_free(f);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    PcapGlobalHeader gh = {
        .magic = 0xa1b2c3d4,
        .version_major = 2,
        .version_minor = 4,
        .thiszone = 0,
        .sigfigs = 0,
        .snaplen = CAP_SNAPLEN,
        .network = 105,
    };
    storage_file_write(f, &gh, sizeof(gh));

    /* Frames: { [msg_num:1][len:2 LE][bytes] } */
    uint32_t ts_us = furi_get_tick() * 1000u;
    uint8_t written = 0;
    for(uint8_t i = 0; i < frame_count; ++i) {
        if(o + 3 > blob_len) break;
        o++; /* msg_num — Reihenfolge im pcap egal */
        uint16_t flen = (uint16_t)(blob[o] | (blob[o + 1] << 8));
        o += 2;
        if(flen == 0 || o + flen > blob_len) break;
        PcapPacketHeader ph = {
            .ts_sec = ts_us / 1000000,
            .ts_usec = ts_us % 1000000,
            .incl_len = flen,
            .orig_len = flen,
        };
        storage_file_write(f, &ph, sizeof(ph));
        storage_file_write(f, &blob[o], flen);
        o += flen;
        ts_us += 1000; /* je Frame +1ms, nur kosmetisch */
        written++;
    }

    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    FURI_LOG_I(TAG, "handshake -> %s (%u frames)", path, written);
    return written > 0;
}
