#pragma once

#include <stdint.h>
#include <storage/storage.h>

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t network; // 105 = LINKTYPE_IEEE802_11
} WlanPcapGlobalHeader;

typedef struct __attribute__((packed)) {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
} WlanPcapPacketHeader;

File* wlan_pcap_open(Storage* storage, const char* path);
void wlan_pcap_write_packet(File* file, uint32_t timestamp_us, const uint8_t* data, uint16_t len);
void wlan_pcap_close(File* file);
