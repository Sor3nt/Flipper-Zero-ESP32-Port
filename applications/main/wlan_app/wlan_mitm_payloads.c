#include "wlan_mitm_payloads.h"

#include <storage/storage.h>
#include <furi.h>
#include <stdio.h>
#include <string.h>

static bool ends_with_js(const char* name) {
    size_t n = strlen(name);
    if(n < 4) return false;
    const char* tail = name + n - 3;
    return (tail[0] == '.') &&
           (tail[1] == 'j' || tail[1] == 'J') &&
           (tail[2] == 's' || tail[2] == 'S');
}

static void copy_basename(const char* file, char* out, size_t out_max) {
    size_t n = strlen(file);
    if(n >= 3) n -= 3; // ".js" wegschneiden
    if(n >= out_max) n = out_max - 1;
    memcpy(out, file, n);
    out[n] = '\0';
}

void wlan_mitm_payloads_scan(WlanMitmPayloadList* list) {
    if(!list) return;
    list->count = 0;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);
    if(storage_dir_open(dir, WLAN_MITM_PAYLOADS_DIR)) {
        char name[64];
        FileInfo info;
        while(list->count < WLAN_MITM_PAYLOAD_MAX &&
              storage_dir_read(dir, &info, name, sizeof(name))) {
            if(info.flags & FSF_DIRECTORY) continue;
            if(!ends_with_js(name)) continue;
            copy_basename(name, list->items[list->count].name, WLAN_MITM_PAYLOAD_NAME_MAX);
            list->count++;
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
}

bool wlan_mitm_payloads_load(const char* name, char* out, size_t out_max) {
    if(!name || !name[0] || !out || out_max < 2) return false;

    char path[96];
    int n = snprintf(path, sizeof(path), "%s/%s.js", WLAN_MITM_PAYLOADS_DIR, name);
    if(n <= 0 || n >= (int)sizeof(path)) return false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint16_t read = storage_file_read(file, out, out_max - 1);
        out[read] = '\0';
        // trailing CR/LF/Whitespace strippen — beim Inject sonst kosmetischer Müll.
        while(read > 0) {
            char c = out[read - 1];
            if(c != '\n' && c != '\r' && c != ' ' && c != '\t') break;
            out[--read] = '\0';
        }
        ok = (read > 0);
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}
