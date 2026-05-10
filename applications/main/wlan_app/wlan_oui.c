#include "wlan_oui.h"

#include <furi.h>
#include <storage/storage.h>
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>

#define WLAN_OUI_PATH "/ext/apps_data/wifi/mac-vendor.txt"
#define WLAN_OUI_NAME_MAX 28
#define WLAN_OUI_FILE_MAX (8 * 1024 * 1024) // 8 MiB Hard-Cap

typedef struct {
    uint32_t oui24;
    char name[WLAN_OUI_NAME_MAX];
} OuiEntry;

struct WlanOuiTable {
    OuiEntry* entries;
    size_t count;
};

static int hex_val(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if(c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

// Parsed die OUI am Zeilenanfang. Ignoriert ':' '-' ' ' '\t' zwischen
// Hex-Ziffern. Returns true und schreibt 24-bit OUI in *out + Anzahl
// konsumierter Bytes nach *consumed.
static bool parse_oui(const char* line, size_t len, uint32_t* out, size_t* consumed) {
    int hex_chars[6];
    int n = 0;
    size_t i = 0;
    while(i < len && n < 6) {
        int v = hex_val(line[i]);
        if(v >= 0) {
            hex_chars[n++] = v;
            i++;
        } else if(line[i] == ':' || line[i] == '-' || line[i] == ' ' || line[i] == '\t') {
            i++;
        } else {
            break;
        }
    }
    if(n != 6) return false;
    *out = ((uint32_t)hex_chars[0] << 20) | ((uint32_t)hex_chars[1] << 16) |
           ((uint32_t)hex_chars[2] << 12) | ((uint32_t)hex_chars[3] << 8) |
           ((uint32_t)hex_chars[4] << 4)  | (uint32_t)hex_chars[5];
    *consumed = i;
    return true;
}

static int oui_compare(const void* a, const void* b) {
    uint32_t oa = ((const OuiEntry*)a)->oui24;
    uint32_t ob = ((const OuiEntry*)b)->oui24;
    if(oa < ob) return -1;
    if(oa > ob) return 1;
    return 0;
}

static char* oui_alloc_buf(size_t bytes) {
    char* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!p) p = malloc(bytes);
    return p;
}

static OuiEntry* oui_alloc_entries(size_t count) {
    size_t bytes = sizeof(OuiEntry) * count;
    OuiEntry* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!p) p = malloc(bytes);
    return p;
}

WlanOuiTable* wlan_oui_load(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    if(!storage_file_open(f, WLAN_OUI_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(f);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    uint64_t size64 = storage_file_size(f);
    if(size64 == 0 || size64 > WLAN_OUI_FILE_MAX) {
        storage_file_close(f);
        storage_file_free(f);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }
    size_t total = (size_t)size64;

    char* file_buf = oui_alloc_buf(total + 1);
    if(!file_buf) {
        storage_file_close(f);
        storage_file_free(f);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    // Storage-API liest in chunks ≤ uint16_t — daher schleifen.
    size_t read_total = 0;
    while(read_total < total) {
        size_t want = total - read_total;
        if(want > 32768) want = 32768;
        uint16_t got = storage_file_read(f, file_buf + read_total, (uint16_t)want);
        if(got == 0) break;
        read_total += got;
    }
    file_buf[read_total] = '\0';
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);

    // Pass 1: Anzahl parsbarer Zeilen ermitteln.
    size_t line_count = 0;
    {
        const char* p = file_buf;
        const char* end = file_buf + read_total;
        while(p < end) {
            const char* nl = memchr(p, '\n', end - p);
            const char* line_end = nl ? nl : end;
            size_t line_len = line_end - p;
            if(line_len > 6 && p[0] != '#') {
                uint32_t oui;
                size_t cons;
                if(parse_oui(p, line_len, &oui, &cons)) line_count++;
            }
            p = nl ? nl + 1 : end;
        }
    }

    if(line_count == 0) {
        free(file_buf);
        return NULL;
    }

    OuiEntry* entries = oui_alloc_entries(line_count);
    if(!entries) {
        free(file_buf);
        return NULL;
    }

    // Pass 2: parsen.
    size_t idx = 0;
    {
        const char* p = file_buf;
        const char* end = file_buf + read_total;
        while(p < end && idx < line_count) {
            const char* nl = memchr(p, '\n', end - p);
            const char* line_end = nl ? nl : end;
            // CR strippen.
            while(line_end > p && (line_end[-1] == '\r' || line_end[-1] == '\n')) line_end--;
            size_t line_len = line_end - p;

            if(line_len > 6 && p[0] != '#') {
                uint32_t oui;
                size_t after_oui = 0;
                if(parse_oui(p, line_len, &oui, &after_oui)) {
                    // Trenner überspringen: ',', '\t', ' ', ';'.
                    while(after_oui < line_len &&
                          (p[after_oui] == ',' || p[after_oui] == '\t' ||
                           p[after_oui] == ' ' || p[after_oui] == ';')) {
                        after_oui++;
                    }
                    // Name bis zum ersten Tab/Komma/Newline.
                    size_t name_end = after_oui;
                    while(name_end < line_len &&
                          p[name_end] != '\t' && p[name_end] != ',' &&
                          p[name_end] != '\r' && p[name_end] != '\n') {
                        name_end++;
                    }
                    // Trailing-Whitespace strippen.
                    while(name_end > after_oui && (p[name_end - 1] == ' ')) name_end--;

                    size_t name_len = name_end - after_oui;
                    if(name_len == 0) {
                        // OUI ohne Namen → überspringen, line_count war Obergrenze.
                        p = nl ? nl + 1 : end;
                        continue;
                    }
                    if(name_len > WLAN_OUI_NAME_MAX - 1) name_len = WLAN_OUI_NAME_MAX - 1;

                    entries[idx].oui24 = oui;
                    memcpy(entries[idx].name, p + after_oui, name_len);
                    entries[idx].name[name_len] = '\0';
                    idx++;
                }
            }
            p = nl ? nl + 1 : end;
        }
    }

    free(file_buf);

    if(idx == 0) {
        free(entries);
        return NULL;
    }

    qsort(entries, idx, sizeof(OuiEntry), oui_compare);

    WlanOuiTable* t = malloc(sizeof(WlanOuiTable));
    t->entries = entries;
    t->count = idx;
    return t;
}

void wlan_oui_free(WlanOuiTable* t) {
    if(!t) return;
    free(t->entries);
    free(t);
}

const char* wlan_oui_lookup(WlanOuiTable* t, const uint8_t mac[6]) {
    if(!t || !t->entries || t->count == 0) return NULL;
    uint32_t key = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | (uint32_t)mac[2];

    size_t lo = 0;
    size_t hi = t->count;
    while(lo < hi) {
        size_t mid = (lo + hi) / 2;
        uint32_t v = t->entries[mid].oui24;
        if(v < key) lo = mid + 1;
        else if(v > key) hi = mid;
        else return t->entries[mid].name;
    }
    return NULL;
}
