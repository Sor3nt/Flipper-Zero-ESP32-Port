#pragma once

#include <stdint.h>
#include <storage/storage.h>

typedef struct __attribute__((packed)) {
    uint32_t magic;         // 0xa1b2c3d4
    uint16_t version_major; // 2
    uint16_t version_minor; // 4
    int32_t thiszone;       // 0
    uint32_t sigfigs;       // 0
    uint32_t snaplen;       // 2500
    uint32_t network;       // 105 = LINKTYPE_IEEE802_11
} PcapGlobalHeader;

typedef struct __attribute__((packed)) {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
} PcapPacketHeader;

/** Open PCAP file and write global header. Returns file handle or NULL. */
File* wifi_pcap_open(Storage* storage, const char* path);

/** Write a packet to the PCAP file. */
void wifi_pcap_write_packet(File* file, uint32_t timestamp_us, const uint8_t* data, uint16_t len);

/** Close PCAP file. */
void wifi_pcap_close(File* file);
