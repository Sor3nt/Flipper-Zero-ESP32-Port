#include "wlan_pcap.h"
#include <esp_log.h>

#define TAG "WlanPcap"

File* wlan_pcap_open(Storage* storage, const char* path) {
    storage_common_mkdir(storage, "/ext/wifi");

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        storage_file_free(file);
        return NULL;
    }

    WlanPcapGlobalHeader header = {
        .magic = 0xa1b2c3d4,
        .version_major = 2,
        .version_minor = 4,
        .thiszone = 0,
        .sigfigs = 0,
        .snaplen = 512,
        .network = 105,
    };
    storage_file_write(file, &header, sizeof(header));
    return file;
}

void wlan_pcap_write_packet(File* file, uint32_t timestamp_us, const uint8_t* data, uint16_t len) {
    if(!file || !data || len == 0) return;
    WlanPcapPacketHeader pkt_hdr = {
        .ts_sec = timestamp_us / 1000000,
        .ts_usec = timestamp_us % 1000000,
        .incl_len = len,
        .orig_len = len,
    };
    storage_file_write(file, &pkt_hdr, sizeof(pkt_hdr));
    storage_file_write(file, data, len);
}

void wlan_pcap_close(File* file) {
    if(file) {
        storage_file_close(file);
        storage_file_free(file);
    }
}
