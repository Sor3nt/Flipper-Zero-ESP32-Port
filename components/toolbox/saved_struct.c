#include "saved_struct.h"
#include "path.h"

#include <furi.h>
#include <string.h>
#include <storage/storage.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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

static uint8_t saved_struct_checksum(const void* data, size_t size) {
    uint8_t checksum = 0;
    const uint8_t* source = data;
    for(size_t i = 0; i < size; i++) {
        checksum += source[i];
    }
    return checksum;
}

/** Open the storage record, but only if it exists and the SD card is mounted.
 *  Returns NULL otherwise (caller must fall back to NVS). */
static Storage* saved_struct_storage_open(void) {
    if(!furi_record_exists(RECORD_STORAGE)) return NULL;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(storage_sd_status(storage) != FSE_OK) {
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }
    return storage;
}

/* ---- File backend (primary) ---- */

static bool saved_struct_file_save(
    Storage* storage,
    const char* path,
    const void* data,
    size_t size,
    uint8_t magic,
    uint8_t version) {
    // Make sure the containing directory exists ("/int" maps to "/sdcard/.int")
    FuriString* dirname = furi_string_alloc();
    path_extract_dirname(path, dirname);
    if(furi_string_size(dirname) > 0) {
        storage_simply_mkdir(storage, furi_string_get_cstr(dirname));
    }
    furi_string_free(dirname);

    SavedStructHeader header = {
        .magic = magic,
        .version = version,
        .checksum = saved_struct_checksum(data, size),
        .flags = 0,
        .timestamp = 0,
    };

    File* file = storage_file_alloc(storage);
    bool result = false;

    if(!storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FURI_LOG_E(
            TAG, "Open failed \"%s\". Error: \'%s\'", path, storage_file_get_error_desc(file));
    } else {
        size_t bytes_count = storage_file_write(file, &header, sizeof(header));
        bytes_count += storage_file_write(file, data, size);

        if(bytes_count != (size + sizeof(header))) {
            FURI_LOG_E(
                TAG, "Write failed \"%s\". Error: \'%s\'", path, storage_file_get_error_desc(file));
        } else {
            result = true;
        }
    }

    storage_file_close(file);
    storage_file_free(file);
    return result;
}

static bool saved_struct_file_load(
    Storage* storage,
    const char* path,
    void* data,
    size_t size,
    uint8_t magic,
    uint8_t version) {
    File* file = storage_file_alloc(storage);
    uint8_t* data_read = malloc(size);
    SavedStructHeader header;
    bool result = false;

    do {
        if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
            FURI_LOG_D(TAG, "No file \"%s\"", path);
            break;
        }

        size_t bytes_count = storage_file_read(file, &header, sizeof(header));
        bytes_count += storage_file_read(file, data_read, size);

        if(bytes_count != (sizeof(header) + size)) {
            FURI_LOG_E(TAG, "Size mismatch of file \"%s\"", path);
            break;
        }

        if(header.magic != magic || header.version != version) {
            FURI_LOG_E(
                TAG,
                "Magic(%d != %d) or Version(%d != %d) mismatch of file \"%s\"",
                header.magic,
                magic,
                header.version,
                version,
                path);
            break;
        }

        if(header.checksum != saved_struct_checksum(data_read, size)) {
            FURI_LOG_E(TAG, "Checksum mismatch of file \"%s\"", path);
            break;
        }

        memcpy(data, data_read, size);
        result = true;
    } while(false);

    storage_file_close(file);
    storage_file_free(file);
    free(data_read);
    return result;
}

static bool saved_struct_file_get_metadata(
    Storage* storage,
    const char* path,
    uint8_t* magic,
    uint8_t* version,
    size_t* payload_size) {
    File* file = storage_file_alloc(storage);
    SavedStructHeader header;
    bool result = false;

    do {
        if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
            FURI_LOG_D(TAG, "No file \"%s\"", path);
            break;
        }

        if(storage_file_read(file, &header, sizeof(header)) != sizeof(header)) {
            FURI_LOG_E(TAG, "Failed to read header of \"%s\"", path);
            break;
        }

        if(magic) *magic = header.magic;
        if(version) *version = header.version;
        if(payload_size) *payload_size = (size_t)storage_file_size(file) - sizeof(header);

        result = true;
    } while(false);

    storage_file_close(file);
    storage_file_free(file);
    return result;
}

/* ---- NVS backend (legacy read / no-SD fallback) ---- */

static bool saved_struct_nvs_read_blob(const char* path, uint8_t** blob_out, size_t* blob_size) {
    char key[16];
    path_to_nvs_key(path, key, sizeof(key));

    nvs_handle_t nvs;
    if(nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;

    size_t size = 0;
    if(nvs_get_blob(nvs, key, NULL, &size) != ESP_OK || size < sizeof(SavedStructHeader)) {
        nvs_close(nvs);
        return false;
    }

    uint8_t* blob = malloc(size);
    if(!blob) {
        nvs_close(nvs);
        return false;
    }

    esp_err_t err = nvs_get_blob(nvs, key, blob, &size);
    nvs_close(nvs);

    if(err != ESP_OK) {
        free(blob);
        return false;
    }

    *blob_out = blob;
    *blob_size = size;
    return true;
}

static bool saved_struct_nvs_load(
    const char* path,
    void* data,
    size_t size,
    uint8_t magic,
    uint8_t version) {
    uint8_t* blob = NULL;
    size_t blob_size = 0;
    if(!saved_struct_nvs_read_blob(path, &blob, &blob_size)) return false;

    bool result = false;
    do {
        if(blob_size != sizeof(SavedStructHeader) + size) break;

        SavedStructHeader header;
        memcpy(&header, blob, sizeof(header));
        if(header.magic != magic || header.version != version) break;

        const uint8_t* payload = blob + sizeof(header);
        if(header.checksum != saved_struct_checksum(payload, size)) break;

        memcpy(data, payload, size);
        result = true;
    } while(false);

    free(blob);
    return result;
}

/* Argument für den NVS-Write-Helper-Task. */
typedef struct {
    const char* path;
    const void* data;
    size_t size;
    uint8_t magic;
    uint8_t version;
    volatile bool result;
    volatile bool done;
} SavedStructNvsWriteCtx;

/* Führt den eigentlichen NVS-Write aus — MUSS auf einem Task mit INTERNEM DRAM-
 * Stack laufen (siehe saved_struct_nvs_save). nvs_commit schaltet den Flash-Cache
 * ab; ein PSRAM-Stack (wie ihn die Furi-Service-Tasks haben) ist dann nicht mehr
 * erreichbar → DoubleException/TG1WDT. Deshalb baut dieser Task Key und Blob
 * lokal/intern auf, sodass während des Cache-disable nur internes RAM berührt
 * wird. Siehe project_psram_stack_nvs_doubleexception. */
static void saved_struct_nvs_write_task(void* p) {
    SavedStructNvsWriteCtx* ctx = p;

    char key[16];
    path_to_nvs_key(ctx->path, key, sizeof(key));

    SavedStructHeader header = {
        .magic = ctx->magic,
        .version = ctx->version,
        .checksum = saved_struct_checksum(ctx->data, ctx->size),
        .flags = 0,
        .timestamp = 0,
    };

    size_t blob_size = sizeof(header) + ctx->size;
    /* Blob MUSS internes DRAM sein — er wird von nvs_set_blob während des
     * Flash-Writes (Cache aus) gelesen. */
    uint8_t* blob = heap_caps_malloc(blob_size, MALLOC_CAP_INTERNAL);
    bool result = false;
    if(blob) {
        memcpy(blob, &header, sizeof(header));
        memcpy(blob + sizeof(header), ctx->data, ctx->size);

        nvs_handle_t nvs;
        if(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            if(nvs_set_blob(nvs, key, blob, blob_size) == ESP_OK && nvs_commit(nvs) == ESP_OK) {
                result = true;
            }
            nvs_close(nvs);
        }
        heap_caps_free(blob);
    } else {
        FURI_LOG_E(TAG, "Failed to allocate internal blob for NVS save");
    }

    ctx->result = result;
    ctx->done = true;
    vTaskDelete(NULL);
}

static bool saved_struct_nvs_save(
    const char* path,
    const void* data,
    size_t size,
    uint8_t magic,
    uint8_t version) {
    FURI_LOG_W(TAG, "No SD card, saving \"%s\" to NVS", path);

    /* Den Write auf einen Task mit internem DRAM-Stack auslagern — plain
     * xTaskCreate liefert hier einen internen Stack (kein SPIRAM_ALLOW_STACK),
     * anders als die PSRAM-gestackten Furi-Service-Tasks, die den nvs_commit
     * sonst mit DoubleException killen würden. */
    SavedStructNvsWriteCtx ctx = {
        .path = path,
        .data = data,
        .size = size,
        .magic = magic,
        .version = version,
        .result = false,
        .done = false,
    };

    if(xTaskCreate(saved_struct_nvs_write_task, "nvs_wr", 3072, &ctx, 5, NULL) != pdPASS) {
        FURI_LOG_E(TAG, "Failed to spawn NVS write task");
        return false;
    }

    while(!ctx.done) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    if(!ctx.result) {
        FURI_LOG_E(TAG, "NVS save failed for \"%s\"", path);
    }
    return ctx.result;
}

/* ---- Public API ---- */

bool saved_struct_save(
    const char* path,
    const void* data,
    size_t size,
    uint8_t magic,
    uint8_t version) {
    furi_check(path);
    furi_check(data);
    furi_check(size > 0);

    FURI_LOG_I(TAG, "Saving \"%s\" (%u bytes)", path, (unsigned)size);

    Storage* storage = saved_struct_storage_open();
    if(!storage) {
        return saved_struct_nvs_save(path, data, size, magic, version);
    }

    bool result = saved_struct_file_save(storage, path, data, size, magic, version);
    furi_record_close(RECORD_STORAGE);
    return result;
}

bool saved_struct_load(const char* path, void* data, size_t size, uint8_t magic, uint8_t version) {
    furi_check(path);
    furi_check(data);
    furi_check(size > 0);

    FURI_LOG_I(TAG, "Loading \"%s\"", path);

    Storage* storage = saved_struct_storage_open();
    if(!storage) {
        return saved_struct_nvs_load(path, data, size, magic, version);
    }

    bool result = saved_struct_file_load(storage, path, data, size, magic, version);

    if(!result && saved_struct_nvs_load(path, data, size, magic, version)) {
        // Legacy settings from an NVS-only firmware — migrate them to the SD card
        FURI_LOG_I(TAG, "Migrating \"%s\" from NVS to SD", path);
        saved_struct_file_save(storage, path, data, size, magic, version);
        result = true;
    }

    furi_record_close(RECORD_STORAGE);
    return result;
}

bool saved_struct_get_metadata(
    const char* path,
    uint8_t* magic,
    uint8_t* version,
    size_t* payload_size) {
    furi_check(path);

    Storage* storage = saved_struct_storage_open();
    bool result = false;

    if(storage) {
        result = saved_struct_file_get_metadata(storage, path, magic, version, payload_size);
        furi_record_close(RECORD_STORAGE);
    }

    if(!result) {
        // Legacy NVS blob
        uint8_t* blob = NULL;
        size_t blob_size = 0;
        if(saved_struct_nvs_read_blob(path, &blob, &blob_size)) {
            SavedStructHeader header;
            memcpy(&header, blob, sizeof(header));
            if(magic) *magic = header.magic;
            if(version) *version = header.version;
            if(payload_size) *payload_size = blob_size - sizeof(header);
            free(blob);
            result = true;
        }
    }

    return result;
}
