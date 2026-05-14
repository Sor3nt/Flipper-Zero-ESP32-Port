#include "wlan_lan_cache.h"

#include <storage/storage.h>
#include <furi.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define LAN_CACHE_DIR_WIFI "/ext/wifi"
#define LAN_CACHE_DIR_LAN  "/ext/wifi/lan"
#define LAN_CACHE_HEADER   "# wlan_app lan cache v1\nip,mac,hostname\n"

static void sanitize_ssid_for_filename(const char* ssid, char* out, size_t out_len) {
    size_t j = 0;
    for(size_t i = 0; ssid[i] && j < out_len - 1; i++) {
        char c = ssid[i];
        if(c == '/' || c == '\\' || c == ':' || c == '*' ||
           c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
        out[j++] = c;
    }
    out[j] = '\0';
}

static void build_lan_cache_path(const char* ssid, char* path, size_t path_len) {
    char safe_ssid[34];
    sanitize_ssid_for_filename(ssid, safe_ssid, sizeof(safe_ssid));
    snprintf(path, path_len, "%s/%s.csv", LAN_CACHE_DIR_LAN, safe_ssid);
}

static bool parse_ip_dotted(const char* s, uint32_t* out_net) {
    unsigned a, b, c, d;
    if(sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if(a > 255 || b > 255 || c > 255 || d > 255) return false;
    uint32_t host = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
    *out_net = __builtin_bswap32(host);
    return true;
}

static bool parse_mac_colon(const char* s, uint8_t out[6]) {
    unsigned m[6];
    if(sscanf(s, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        return false;
    }
    for(int i = 0; i < 6; i++) {
        if(m[i] > 0xFF) return false;
        out[i] = (uint8_t)m[i];
    }
    return true;
}

bool wlan_lan_cache_exists(const char* ssid) {
    if(!ssid || !ssid[0]) return false;

    char path[80];
    build_lan_cache_path(ssid, path, sizeof(path));

    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool exists = (storage_common_stat(storage, path, NULL) == FSE_OK);
    furi_record_close(RECORD_STORAGE);

    return exists;
}

bool wlan_lan_cache_save(
    const char* ssid,
    const WlanDeviceRecord* devices,
    uint16_t count) {
    if(!ssid || !ssid[0] || (!devices && count > 0)) return false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, LAN_CACHE_DIR_WIFI);
    storage_simply_mkdir(storage, LAN_CACHE_DIR_LAN);

    char path[80];
    build_lan_cache_path(ssid, path, sizeof(path));

    File* file = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        size_t hlen = strlen(LAN_CACHE_HEADER);
        ok = (storage_file_write(file, LAN_CACHE_HEADER, hlen) == hlen);

        char line[96];
        for(uint16_t i = 0; ok && i < count; i++) {
            const WlanDeviceRecord* d = &devices[i];
            uint32_t host = __builtin_bswap32(d->ip);
            uint8_t a = (host >> 24) & 0xFF;
            uint8_t b = (host >> 16) & 0xFF;
            uint8_t c = (host >> 8) & 0xFF;
            uint8_t e = host & 0xFF;

            char hostname_buf[WLAN_APP_HOSTNAME_MAX];
            size_t hi = 0;
            for(size_t j = 0; j < sizeof(d->hostname) && d->hostname[j]; j++) {
                char ch = d->hostname[j];
                if(ch == ',' || ch == '\n' || ch == '\r') ch = '_';
                if(hi < sizeof(hostname_buf) - 1) hostname_buf[hi++] = ch;
            }
            hostname_buf[hi] = '\0';

            int n = snprintf(
                line,
                sizeof(line),
                "%u.%u.%u.%u,%02x:%02x:%02x:%02x:%02x:%02x,%s\n",
                a, b, c, e,
                d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5],
                hostname_buf);
            if(n <= 0 || n >= (int)sizeof(line)) {
                ok = false;
                break;
            }
            ok = (storage_file_write(file, line, (size_t)n) == (size_t)n);
        }
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    return ok;
}

static bool parse_line(const char* line, WlanDeviceRecord* out) {
    while(*line == ' ' || *line == '\t') line++;
    if(*line == '\0' || *line == '#') return false;

    char buf[160];
    size_t len = 0;
    while(line[len] && line[len] != '\n' && line[len] != '\r' && len < sizeof(buf) - 1) {
        buf[len] = line[len];
        len++;
    }
    buf[len] = '\0';

    char* p = buf;
    char* comma1 = strchr(p, ',');
    if(!comma1) return false;
    *comma1 = '\0';
    char* ip_str = p;

    char* mac_str = comma1 + 1;
    char* comma2 = strchr(mac_str, ',');
    char* hostname_str = NULL;
    if(comma2) {
        *comma2 = '\0';
        hostname_str = comma2 + 1;
    }

    if(strcmp(ip_str, "ip") == 0) return false;

    uint32_t ip_net = 0;
    if(!parse_ip_dotted(ip_str, &ip_net)) return false;

    uint8_t mac[6];
    if(!parse_mac_colon(mac_str, mac)) return false;

    memset(out, 0, sizeof(*out));
    out->ip = ip_net;
    memcpy(out->mac, mac, 6);
    if(hostname_str && hostname_str[0]) {
        strncpy(out->hostname, hostname_str, sizeof(out->hostname) - 1);
    }
    return true;
}

bool wlan_lan_cache_load(
    const char* ssid,
    WlanDeviceRecord* out_devices,
    uint16_t max,
    uint16_t* out_count) {
    if(!ssid || !ssid[0] || !out_devices || max == 0 || !out_count) return false;
    *out_count = 0;

    char path[80];
    build_lan_cache_path(ssid, path, sizeof(path));

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    bool opened = false;
    char* content = NULL;
    do {
        if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) break;
        opened = true;

        uint64_t size = storage_file_size(file);
        if(size == 0 || size > 64 * 1024) break;

        content = malloc((size_t)size + 1);
        if(!content) break;

        uint16_t read = storage_file_read(file, content, (uint16_t)size);
        if(read == 0) break;
        content[read] = '\0';

        char* cursor = content;
        while(*cursor && *out_count < max) {
            char* nl = strchr(cursor, '\n');
            if(nl) *nl = '\0';

            WlanDeviceRecord rec;
            if(parse_line(cursor, &rec)) {
                out_devices[*out_count] = rec;
                (*out_count)++;
            }

            if(!nl) break;
            cursor = nl + 1;
        }
    } while(0);

    if(content) free(content);
    if(opened) storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    return (*out_count > 0);
}
