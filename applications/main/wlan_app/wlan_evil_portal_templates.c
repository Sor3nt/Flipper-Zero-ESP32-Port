#include "wlan_evil_portal_templates.h"

#include <storage/storage.h>
#include <furi.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Sicherheitslimit für eine einzelne Template-Datei. Builtin-Templates sind
// ~3-6 KB; SD-Templates dürfen größer sein, aber nicht den Heap sprengen.
#define WLAN_EP_TEMPLATE_FILE_MAX (32 * 1024)

static bool ends_with_html(const char* name) {
    size_t n = strlen(name);
    if(n < 6) return false; // "x.html"
    const char* t = name + n - 5;
    return t[0] == '.' &&
           (t[1] == 'h' || t[1] == 'H') &&
           (t[2] == 't' || t[2] == 'T') &&
           (t[3] == 'm' || t[3] == 'M') &&
           (t[4] == 'l' || t[4] == 'L');
}

static void copy_basename(const char* file, char* out, size_t out_max) {
    size_t n = strlen(file);
    if(n >= 5) n -= 5; // ".html" wegschneiden
    if(n >= out_max) n = out_max - 1;
    memcpy(out, file, n);
    out[n] = '\0';
}

static void scan_dir(
    Storage* storage, const char* dir_path, bool is_router,
    WlanEvilPortalTemplateList* list) {
    File* dir = storage_file_alloc(storage);
    if(storage_dir_open(dir, dir_path)) {
        char name[64];
        FileInfo info;
        while(list->count < WLAN_EP_TEMPLATE_MAX &&
              storage_dir_read(dir, &info, name, sizeof(name))) {
            if(info.flags & FSF_DIRECTORY) continue;
            if(!ends_with_html(name)) continue;
            WlanEvilPortalTemplateEntry* e = &list->items[list->count];
            copy_basename(name, e->name, WLAN_EP_TEMPLATE_NAME_MAX);
            e->is_router = is_router;
            list->count++;
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
}

void wlan_evil_portal_templates_scan(WlanEvilPortalTemplateList* list) {
    if(!list) return;
    list->count = 0;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    scan_dir(storage, WLAN_EP_LOGIN_TEMPLATE_DIR, false, list);
    scan_dir(storage, WLAN_EP_ROUTER_TEMPLATE_DIR, true, list);
    furi_record_close(RECORD_STORAGE);
}

char* wlan_evil_portal_templates_load(
    const WlanEvilPortalTemplateEntry* entry, size_t* out_len) {
    if(out_len) *out_len = 0;
    if(!entry || !entry->name[0]) return NULL;

    char path[128];
    int n = snprintf(
        path, sizeof(path), "%s/%s.html",
        entry->is_router ? WLAN_EP_ROUTER_TEMPLATE_DIR : WLAN_EP_LOGIN_TEMPLATE_DIR,
        entry->name);
    if(n <= 0 || n >= (int)sizeof(path)) return NULL;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    char* buf = NULL;

    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint64_t fsize = storage_file_size(file);
        if(fsize > 0 && fsize <= WLAN_EP_TEMPLATE_FILE_MAX) {
            size_t len = (size_t)fsize;
            // PSRAM: interner DRAM ist mit WiFi+httpd extrem knapp; das
            // Template (mehrere KB) darf nicht den internen Heap belegen.
            buf = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if(!buf) buf = malloc(len + 1); // Fallback (Board ohne PSRAM)
            if(buf) {
                size_t total = 0;
                while(total < len) {
                    size_t chunk = (len - total > 512) ? 512 : (len - total);
                    size_t r = storage_file_read(file, buf + total, chunk);
                    if(r == 0) break;
                    total += r;
                }
                buf[total] = '\0';
                if(total == 0) {
                    free(buf);
                    buf = NULL;
                } else if(out_len) {
                    *out_len = total;
                }
            }
        }
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return buf;
}
