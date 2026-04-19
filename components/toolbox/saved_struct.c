#include "saved_struct.h"

#include <furi.h>
#include <string.h>
#include <nvs_flash.h>
#include <nvs.h>

#define TAG "SavedStruct"
#define NVS_NAMESPACE "saved_struct"

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t checksum;
    uint8_t flags;
    uint32_t timestamp;
} SavedStructHeader;

/** Convert a file path like "/int/.power.settings" to a short NVS key (max 15 chars).
 *  Takes the basename, strips leading dots, truncates to 15 chars. */
static void path_to_nvs_key(const char* path, char* key, size_t key_size) {
    // Find last '/' to get basename
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;

    // Skip leading dots
    while(*base == '.') base++;

    // Copy up to key_size-1 chars (NVS key max is 15)
    size_t i = 0;
    for(; base[i] && i < key_size - 1; i++) {
        char c = base[i];
        // Replace dots and special chars with underscores
        if(c == '.' || c == ' ' || c == '/') c = '_';
        key[i] = c;
    }
    key[i] = '\0';
}

bool saved_struct_save(
    const char* path,
    const void* data,
    size_t size,
    uint8_t magic,
    uint8_t version) {
    furi_check(path);
    furi_check(data);
    furi_check(size > 0);

    char key[16];
    path_to_nvs_key(path, key, sizeof(key));

    FURI_LOG_I(TAG, "Saving \"%s\" -> NVS key \"%s\" (%u bytes)", path, key, (unsigned)size);

    // Build header + data blob
    SavedStructHeader header = {
        .magic = magic,
        .version = version,
        .flags = 0,
        .timestamp = 0,
    };

    // Calculate checksum
    uint8_t checksum = 0;
    const uint8_t* src = data;
    for(size_t i = 0; i < size; i++) {
        checksum += src[i];
    }
    header.checksum = checksum;

    size_t blob_size = sizeof(header) + size;
    uint8_t* blob = malloc(blob_size);
    if(!blob) {
        FURI_LOG_E(TAG, "Failed to allocate blob for save");
        return false;
    }
    memcpy(blob, &header, sizeof(header));
    memcpy(blob + sizeof(header), data, size);

    nvs_handle_t nvs;
    bool result = false;
    if(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        if(nvs_set_blob(nvs, key, blob, blob_size) == ESP_OK) {
            if(nvs_commit(nvs) == ESP_OK) {
                result = true;
            }
        }
        nvs_close(nvs);
    }

    free(blob);

    if(!result) {
        FURI_LOG_E(TAG, "NVS save failed for \"%s\"", key);
    }
    return result;
}

bool saved_struct_load(const char* path, void* data, size_t size, uint8_t magic, uint8_t version) {
    furi_check(path);
    furi_check(data);
    furi_check(size > 0);

    char key[16];
    path_to_nvs_key(path, key, sizeof(key));

    FURI_LOG_I(TAG, "Loading \"%s\" -> NVS key \"%s\"", path, key);

    nvs_handle_t nvs;
    bool result = false;

    if(nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        FURI_LOG_W(TAG, "NVS namespace not found");
        return false;
    }

    // Get blob size
    size_t expected_size = sizeof(SavedStructHeader) + size;
    size_t blob_size = 0;
    esp_err_t err = nvs_get_blob(nvs, key, NULL, &blob_size);
    if(err != ESP_OK) {
        FURI_LOG_W(TAG, "Key \"%s\" not found in NVS", key);
        nvs_close(nvs);
        return false;
    }

    if(blob_size != expected_size) {
        FURI_LOG_E(TAG, "Size mismatch for \"%s\": got %u, expected %u",
                   key, (unsigned)blob_size, (unsigned)expected_size);
        nvs_close(nvs);
        return false;
    }

    uint8_t* blob = malloc(blob_size);
    if(!blob) {
        nvs_close(nvs);
        return false;
    }

    err = nvs_get_blob(nvs, key, blob, &blob_size);
    nvs_close(nvs);

    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "NVS read failed for \"%s\"", key);
        free(blob);
        return false;
    }

    // Parse header
    SavedStructHeader header;
    memcpy(&header, blob, sizeof(header));

    if(header.magic != magic || header.version != version) {
        FURI_LOG_E(TAG, "Magic(%u!=%u) or Version(%u!=%u) mismatch for \"%s\"",
                   header.magic, magic, header.version, version, key);
        free(blob);
        return false;
    }

    // Verify checksum
    const uint8_t* payload = blob + sizeof(header);
    uint8_t checksum = 0;
    for(size_t i = 0; i < size; i++) {
        checksum += payload[i];
    }

    if(header.checksum != checksum) {
        FURI_LOG_E(TAG, "Checksum mismatch for \"%s\"", key);
        free(blob);
        return false;
    }

    memcpy(data, payload, size);
    free(blob);
    result = true;

    return result;
}

bool saved_struct_get_metadata(
    const char* path,
    uint8_t* magic,
    uint8_t* version,
    size_t* payload_size) {
    furi_check(path);

    char key[16];
    path_to_nvs_key(path, key, sizeof(key));

    nvs_handle_t nvs;
    if(nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    size_t blob_size = 0;
    esp_err_t err = nvs_get_blob(nvs, key, NULL, &blob_size);
    if(err != ESP_OK || blob_size < sizeof(SavedStructHeader)) {
        nvs_close(nvs);
        return false;
    }

    SavedStructHeader header;
    uint8_t* blob = malloc(blob_size);
    if(!blob) {
        nvs_close(nvs);
        return false;
    }

    err = nvs_get_blob(nvs, key, blob, &blob_size);
    nvs_close(nvs);

    if(err != ESP_OK) {
        free(blob);
        return false;
    }

    memcpy(&header, blob, sizeof(header));
    free(blob);

    if(magic) *magic = header.magic;
    if(version) *version = header.version;
    if(payload_size) *payload_size = blob_size - sizeof(SavedStructHeader);

    return true;
}
