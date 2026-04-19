#include "wifi_passwords.h"
#include <storage/storage.h>
#include <furi.h>
#include <string.h>
#include <stdio.h>

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

static void build_password_path(const char* ssid, char* path, size_t path_len) {
    char safe_ssid[34];
    sanitize_ssid_for_filename(ssid, safe_ssid, sizeof(safe_ssid));
    snprintf(path, path_len, "/ext/wifi/%s.txt", safe_ssid);
}

bool wifi_password_exists(const char* ssid) {
    if(!ssid || !ssid[0]) return false;

    char path[64];
    build_password_path(ssid, path, sizeof(path));

    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool exists = (storage_common_stat(storage, path, NULL) == FSE_OK);
    furi_record_close(RECORD_STORAGE);

    return exists;
}

bool wifi_password_save(const char* ssid, const char* password) {
    if(!ssid || !ssid[0] || !password || !password[0]) return false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, "/ext/wifi");

    char path[64];
    build_password_path(ssid, path, sizeof(path));

    File* file = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        size_t len = strlen(password);
        ok = (storage_file_write(file, password, len) == len);
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    return ok;
}

bool wifi_password_read(const char* ssid, char* out_pass, size_t max_len) {
    if(!ssid || !ssid[0] || !out_pass || max_len == 0) return false;

    char path[64];
    build_password_path(ssid, path, sizeof(path));

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    bool ok = false;
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint16_t read = storage_file_read(file, out_pass, max_len - 1);
        out_pass[read] = '\0';

        // Trim trailing whitespace
        while(read > 0 && (out_pass[read - 1] == '\n' || out_pass[read - 1] == '\r' ||
                           out_pass[read - 1] == ' ')) {
            out_pass[--read] = '\0';
        }

        ok = (read > 0);
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    return ok;
}
