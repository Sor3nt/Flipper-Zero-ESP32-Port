#include "flipper_application.h"
#include "elf/elf_file.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include <esp_intr_alloc.h> /* esp_intr_noniram_disable/enable */
#endif

#include <toolbox/path.h>

#include <esp_heap_caps.h>

#include <string.h>

#define TAG "Fap"

struct FlipperApplication {
    ELFDebugInfo state;
    FlipperApplicationManifest manifest;
    ELFFile* elf;
    FuriThread* thread;
    void* ep_thread_args;
};

FlipperApplication*
    flipper_application_alloc(Storage* storage, const ElfApiInterface* api_interface) {
    furi_check(storage);

    FlipperApplication* app = calloc(1, sizeof(FlipperApplication));
    furi_check(app);

    app->elf = elf_file_alloc(storage, api_interface);
    if(!app->elf) {
        free(app);
        return NULL;
    }

    app->thread = NULL;
    app->ep_thread_args = NULL;

    return app;
}

bool flipper_application_is_plugin(FlipperApplication* app) {
    furi_check(app);
    return app->manifest.stack_size == 0;
}

void flipper_application_free(FlipperApplication* app) {
    if(!app) return;

    if(app->thread) {
        furi_thread_join(app->thread);
        furi_thread_free(app->thread);
    }

    elf_file_clear_debug_info(&app->state);

    if(elf_file_is_init_complete(app->elf)) {
        elf_file_call_fini(app->elf);
    }

    elf_file_free(app->elf);

    if(app->ep_thread_args) {
        free(app->ep_thread_args);
        app->ep_thread_args = NULL;
    }

    free(app);
}

static FlipperApplicationPreloadStatus flipper_application_validate_manifest(
    FlipperApplication* app) {
    furi_check(app);

    if(!flipper_application_manifest_is_valid(&app->manifest)) {
        return FlipperApplicationPreloadStatusInvalidManifest;
    }

    if(!flipper_application_manifest_is_target_compatible(&app->manifest)) {
        return FlipperApplicationPreloadStatusTargetMismatch;
    }

    const ElfApiInterface* api_interface = elf_file_get_api_interface(app->elf);
    if(api_interface) {
        if(!flipper_application_manifest_is_too_old(&app->manifest, api_interface)) {
            return FlipperApplicationPreloadStatusApiTooOld;
        }

        if(!flipper_application_manifest_is_too_new(&app->manifest, api_interface)) {
            return FlipperApplicationPreloadStatusApiTooNew;
        }
    }

    return FlipperApplicationPreloadStatusSuccess;
}

static bool flipper_application_process_manifest_section(
    File* file,
    size_t offset,
    size_t size,
    void* context) {
    FlipperApplicationManifest* manifest = context;

    if(size < sizeof(FlipperApplicationManifest)) {
        return false;
    }

    if(manifest == NULL) {
        return true;
    }

    return storage_file_seek(file, offset, true) &&
           storage_file_read(file, manifest, sizeof(FlipperApplicationManifest)) ==
               sizeof(FlipperApplicationManifest);
}

static FlipperApplicationPreloadStatus
    flipper_application_load(FlipperApplication* app, const char* path, bool load_full) {
    FURI_LOG_I(TAG, "Loading FAP: %s (full=%d)", path, load_full);

    if(!elf_file_open(app->elf, path)) {
        FURI_LOG_E(TAG, "elf_file_open failed for %s", path);
        return FlipperApplicationPreloadStatusInvalidFile;
    }

    // if we are loading full file
    if(load_full) {
        FURI_LOG_I(TAG, "Loading section table...");
        ElfLoadSectionTableResult load_result = elf_file_load_section_table(app->elf);
        if(load_result == ElfLoadSectionTableResultError) {
            FURI_LOG_E(TAG, "Section table load failed");
            return FlipperApplicationPreloadStatusInvalidFile;
        } else if(load_result == ElfLoadSectionTableResultNoMemory) {
            FURI_LOG_E(TAG, "Not enough memory for section table");
            return FlipperApplicationPreloadStatusNotEnoughMemory;
        }
        FURI_LOG_I(TAG, "Section table loaded OK");
    }

    // load manifest section
    FURI_LOG_I(TAG, "Looking for .fapmeta section...");
    ElfProcessSectionResult meta_result = elf_process_section(
        app->elf, ".fapmeta", flipper_application_process_manifest_section, &app->manifest);
    if(meta_result != ElfProcessSectionResultSuccess) {
        FURI_LOG_E(TAG, ".fapmeta section result: %d (0=NotFound, 1=CannotProcess, 2=Success)", meta_result);
        return FlipperApplicationPreloadStatusInvalidFile;
    }

    FURI_LOG_I(
        TAG,
        "Manifest: magic=0x%08lX ver=%lu api=%u.%u target=%u stack=%u name='%s'",
        (unsigned long)app->manifest.base.manifest_magic,
        (unsigned long)app->manifest.base.manifest_version,
        app->manifest.base.api_version.major,
        app->manifest.base.api_version.minor,
        app->manifest.base.hardware_target_id,
        app->manifest.stack_size,
        app->manifest.name);

    return flipper_application_validate_manifest(app);
}

FlipperApplicationPreloadStatus
    flipper_application_preload_manifest(FlipperApplication* app, const char* path) {
    furi_check(app);
    furi_check(path);

    return flipper_application_load(app, path, false);
}

FlipperApplicationPreloadStatus flipper_application_preload(FlipperApplication* app, const char* path) {
    furi_check(app);
    furi_check(path);

    return flipper_application_load(app, path, true);
}

const FlipperApplicationManifest* flipper_application_get_manifest(FlipperApplication* app) {
    furi_check(app);

    return &app->manifest;
}

FlipperApplicationLoadStatus flipper_application_map_to_memory(FlipperApplication* app) {
    furi_check(app);

    ELFFileLoadStatus status = elf_file_load_sections(app->elf);

    switch(status) {
    case ELFFileLoadStatusSuccess:
        elf_file_init_debug_info(app->elf, &app->state);

        /* Cache coherency: relocated values were written via data cache.
         * 1. Write back data cache to PSRAM (so PSRAM has the new values)
         * 2. Invalidate instruction cache (so CPU fetches fresh code from PSRAM)
         * ROM functions Cache_WriteBack_All / Cache_Invalidate_ICache_All are only
         * present on Xtensa ESP32-S3.  Skip on RISC-V targets (C6 etc.). */
#if defined(CONFIG_IDF_TARGET_ESP32S3)
        extern void Cache_WriteBack_All(void);
        extern void Cache_Invalidate_ICache_All(void);
        /* The ROM Cache_* helpers briefly disable the cache. Any non-IRAM ISR
         * firing in that window faults with "Cache disabled but cached memory
         * region accessed" (seen as an RMT TX ISR crash during plugin load).
         * Mask non-IRAM interrupts across the cache ops — the same guard the
         * IDF flash driver uses for cache-disabled sections. */
        esp_intr_noniram_disable();
        Cache_WriteBack_All();
        Cache_Invalidate_ICache_All();
        esp_intr_noniram_enable();
        FURI_LOG_I(TAG, "Cache flushed: DCache writeback + ICache invalidate");
#endif

        return FlipperApplicationLoadStatusSuccess;
    case ELFFileLoadStatusMissingImports:
        return FlipperApplicationLoadStatusMissingImports;
    default:
        return FlipperApplicationLoadStatusUnspecifiedError;
    }
}

static int32_t flipper_application_thread(void* context) {
    furi_check(context);
    FlipperApplication* app = (FlipperApplication*)context;

    FURI_LOG_I(TAG, "FAP thread started, calling init arrays...");
    elf_file_call_init(app->elf);

    FlipperApplicationEntryPoint entry_point = elf_file_get_entry_point(app->elf);
    FURI_LOG_I(TAG, "FAP entry point: %p, calling...", entry_point);

    /* Dump literal pool (first 64 bytes of .text data bus) to verify relocations */
    uint32_t entry_addr = (uint32_t)entry_point;
    uint32_t data_base = entry_addr;
    if(data_base >= 0x42000000 && data_base < 0x44000000) {
        data_base -= 0x06000000;
    }
    /* The literal pool is at the START of .text, before the entry point */
    /* Find .text base by going back from entry point */
    /* For now, just dump what's around the entry point's data bus mirror */
    /* Actually, we know the text section start from relocation */
    /* Let's just look at the first 64 bytes of the loaded .text */
    FURI_LOG_I(TAG, "=== Literal pool dump (data bus, first 16 words) ===");
    /* The text section data pointer is entry minus entry_offset */
    /* entry_offset is the ELF e_entry field */
    /* We can compute: text_data = entry_data - elf_entry_offset */
    /* But we don't have elf_entry_offset here. Let's dump relative to data_base */
    /* Scan backwards to find likely start of .text (aligned to 4) */
    volatile uint32_t* dp = (volatile uint32_t*)(data_base - 256); /* ~256 bytes before entry */
    for(int i = 0; i < 16; i++) {
        FURI_LOG_I(TAG, "  [%p] = 0x%08lX", (void*)(dp + i), (unsigned long)dp[i]);
    }

    /* Test: call furi_record_open from FIRMWARE context to verify it works */
    FURI_LOG_I(TAG, "Test: furi_record_open from firmware...");
    void* test_gui = furi_record_open("gui");
    FURI_LOG_I(TAG, "Test: furi_record_open OK: %p", test_gui);
    furi_record_close("gui");
    FURI_LOG_I(TAG, "Test: furi_record_close OK");

    FURI_LOG_I(TAG, "Calling FAP entry point %p", entry_point);
    int32_t ret_code = entry_point(app->ep_thread_args);

    FURI_LOG_I(TAG, "FAP returned: %ld, calling fini arrays...", (long)ret_code);
    elf_file_call_fini(app->elf);

    return ret_code;
}

FuriThread* flipper_application_alloc_thread(FlipperApplication* app, const char* args) {
    furi_check(app);
    furi_check(app->thread == NULL);
    furi_check(!flipper_application_is_plugin(app));

    if(app->ep_thread_args) {
        free(app->ep_thread_args);
    }

    if(args) {
        app->ep_thread_args = strdup(args);
    } else {
        app->ep_thread_args = NULL;
    }

    const FlipperApplicationManifest* manifest = flipper_application_get_manifest(app);
    app->thread = furi_thread_alloc_ex(
        manifest->name, manifest->stack_size, flipper_application_thread, app);

    return app->thread;
}

const FlipperAppPluginDescriptor*
    flipper_application_plugin_get_descriptor(FlipperApplication* app) {
    furi_check(app);

    if(!flipper_application_is_plugin(app)) {
        return NULL;
    }

    if(!elf_file_is_init_complete(app->elf)) {
        elf_file_call_init(app->elf);
    }

    typedef const FlipperAppPluginDescriptor* (*get_lib_descriptor_t)(void);
    get_lib_descriptor_t lib_ep = elf_file_get_entry_point(app->elf);
    furi_check(lib_ep);

    const FlipperAppPluginDescriptor* lib_descriptor = lib_ep();

    FURI_LOG_D(
        TAG,
        "Library for %s, API v. %lu loaded",
        lib_descriptor->appid,
        lib_descriptor->ep_api_version);

    return lib_descriptor;
}

const char* flipper_application_preload_status_to_string(FlipperApplicationPreloadStatus status) {
    switch(status) {
    case FlipperApplicationPreloadStatusSuccess:
        return "Success";
    case FlipperApplicationPreloadStatusInvalidFile:
        return "Invalid file";
    case FlipperApplicationPreloadStatusNotEnoughMemory:
        return "Not enough memory";
    case FlipperApplicationPreloadStatusInvalidManifest:
        return "Invalid file manifest";
    case FlipperApplicationPreloadStatusApiTooOld:
        return "Update Application to use with this Firmware (ApiTooOld)";
    case FlipperApplicationPreloadStatusApiTooNew:
        return "Update Firmware to use with this Application (ApiTooNew)";
    case FlipperApplicationPreloadStatusTargetMismatch:
        return "Hardware target mismatch";
    default:
        return "Unknown error";
    }
}

const char* flipper_application_load_status_to_string(FlipperApplicationLoadStatus status) {
    switch(status) {
    case FlipperApplicationLoadStatusSuccess:
        return "Success";
    case FlipperApplicationLoadStatusUnspecifiedError:
        return "Unknown error";
    case FlipperApplicationLoadStatusMissingImports:
        return "Update Firmware to use with this Application (MissingImports)";
    default:
        return "Unknown error";
    }
}

/* -------------------------------------------------------------------------- */
/* FAP name/icon cache                                                        */
/*                                                                            */
/* Preloading a .fap manifest (open file + parse ELF + read .fapmeta) costs   */
/* ~15-40 ms per file on the SD card. Browsing a folder full of FAPs repeats  */
/* this for every entry on every open, which is why the app list is slow with */
/* many apps. This memoises the extracted name + 10x10 icon per path, keyed   */
/* by file size, in a persistent SD file so warm opens skip the ELF parse.    */
/* Any failure (no SD, PSRAM alloc, corrupt file) transparently falls back to */
/* parsing, so the cache can only ever speed things up, never break them.     */
/* -------------------------------------------------------------------------- */

#define FAP_ICON_CACHE_DIR      EXT_PATH("apps_data")
#define FAP_ICON_CACHE_PATH     EXT_PATH("apps_data/.fap_icon_cache")
#define FAP_ICON_CACHE_MAGIC    0x31434946u /* 'FIC1' */
#define FAP_ICON_CACHE_PATH_MAX 128
#define FAP_ICON_CACHE_INIT_CAP 32

typedef struct {
    uint32_t magic;
    uint32_t record_size;
} FapIconCacheHeader;

typedef struct {
    char path[FAP_ICON_CACHE_PATH_MAX];
    uint32_t size; /* file size = invalidation key */
    uint8_t has_icon;
    char name[FAP_MANIFEST_MAX_APP_NAME_LENGTH];
    uint8_t icon[FAP_MANIFEST_MAX_ICON_SIZE];
} FapIconCacheRecord;

static struct {
    FuriMutex* mutex;
    FapIconCacheRecord* records; /* PSRAM-backed dynamic array */
    size_t count;
    size_t capacity;
    bool loaded; /* SD load attempted */
    bool disabled; /* alloc failure -> bypass cache entirely */
} fap_icon_cache;

static void fap_icon_cache_ensure_mutex(void) {
    /* The very first call happens single-threaded at boot (only one file
     * browser runs at a time), so lazy init without an outer lock is safe. */
    if(!fap_icon_cache.mutex) {
        fap_icon_cache.mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    }
}

static int fap_icon_cache_find_locked(const char* path) {
    for(size_t i = 0; i < fap_icon_cache.count; i++) {
        if(strncmp(fap_icon_cache.records[i].path, path, FAP_ICON_CACHE_PATH_MAX) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Insert/update a record in the RAM array. Returns true if it replaced an
 * existing entry (same path), false if it was appended as new. */
static bool fap_icon_cache_upsert_ram_locked(const FapIconCacheRecord* rec) {
    int idx = fap_icon_cache_find_locked(rec->path);
    if(idx >= 0) {
        fap_icon_cache.records[idx] = *rec;
        return true;
    }
    if(fap_icon_cache.count >= fap_icon_cache.capacity) {
        size_t new_cap =
            fap_icon_cache.capacity ? fap_icon_cache.capacity * 2 : FAP_ICON_CACHE_INIT_CAP;
        FapIconCacheRecord* grown = heap_caps_realloc(
            fap_icon_cache.records,
            new_cap * sizeof(FapIconCacheRecord),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if(!grown) {
            fap_icon_cache.disabled = true;
            return false;
        }
        fap_icon_cache.records = grown;
        fap_icon_cache.capacity = new_cap;
    }
    fap_icon_cache.records[fap_icon_cache.count++] = *rec;
    return false;
}

/* Truncate + write the whole cache (used for compaction and corrupt-file reset). */
static void fap_icon_cache_rewrite_file_locked(Storage* storage) {
    storage_simply_mkdir(storage, FAP_ICON_CACHE_DIR);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, FAP_ICON_CACHE_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FapIconCacheHeader hdr = {FAP_ICON_CACHE_MAGIC, sizeof(FapIconCacheRecord)};
        storage_file_write(file, &hdr, sizeof(hdr));
        for(size_t i = 0; i < fap_icon_cache.count; i++) {
            storage_file_write(file, &fap_icon_cache.records[i], sizeof(FapIconCacheRecord));
        }
    }
    storage_file_close(file);
    storage_file_free(file);
}

static void fap_icon_cache_append_file_locked(Storage* storage, const FapIconCacheRecord* rec) {
    storage_simply_mkdir(storage, FAP_ICON_CACHE_DIR);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, FAP_ICON_CACHE_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        if(storage_file_size(file) == 0) {
            FapIconCacheHeader hdr = {FAP_ICON_CACHE_MAGIC, sizeof(FapIconCacheRecord)};
            storage_file_write(file, &hdr, sizeof(hdr));
        }
        storage_file_write(file, rec, sizeof(FapIconCacheRecord));
    }
    storage_file_close(file);
    storage_file_free(file);
}

static void fap_icon_cache_load_locked(Storage* storage) {
    if(fap_icon_cache.loaded || fap_icon_cache.disabled) return;
    fap_icon_cache.loaded = true;

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, FAP_ICON_CACHE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_close(file);
        storage_file_free(file);
        return; /* no cache file yet -> stays empty, created on first put() */
    }

    FapIconCacheHeader hdr = {0};
    bool valid = storage_file_read(file, &hdr, sizeof(hdr)) == sizeof(hdr) &&
                 hdr.magic == FAP_ICON_CACHE_MAGIC &&
                 hdr.record_size == sizeof(FapIconCacheRecord);

    size_t raw = 0;
    if(valid) {
        FapIconCacheRecord rec;
        while(storage_file_read(file, &rec, sizeof(rec)) == sizeof(rec)) {
            rec.path[FAP_ICON_CACHE_PATH_MAX - 1] = '\0'; /* guard against corrupt paths */
            raw++;
            if(fap_icon_cache.disabled) break;
            fap_icon_cache_upsert_ram_locked(&rec); /* dedup on load: last record wins */
        }
    }
    storage_file_close(file);
    storage_file_free(file);

    if(!valid) {
        /* Corrupt/old-format file -> reset to a clean empty cache so appends work. */
        fap_icon_cache_rewrite_file_locked(storage);
    } else if(!fap_icon_cache.disabled && raw > fap_icon_cache.count) {
        /* Stale duplicates from in-place updates accumulated -> compact on disk. */
        fap_icon_cache_rewrite_file_locked(storage);
    }

    FURI_LOG_D(TAG, "Icon cache loaded: %u entries", (unsigned)fap_icon_cache.count);
}

static bool fap_icon_cache_try_get(
    Storage* storage,
    const char* path,
    uint32_t size,
    uint8_t** icon_ptr,
    FuriString* item_name) {
    fap_icon_cache_ensure_mutex();
    if(!fap_icon_cache.mutex) return false;

    bool hit = false;
    furi_mutex_acquire(fap_icon_cache.mutex, FuriWaitForever);
    fap_icon_cache_load_locked(storage);
    if(!fap_icon_cache.disabled) {
        int idx = fap_icon_cache_find_locked(path);
        if(idx >= 0 && fap_icon_cache.records[idx].size == size) {
            const FapIconCacheRecord* rec = &fap_icon_cache.records[idx];
            if(rec->has_icon && *icon_ptr) {
                memcpy(*icon_ptr, rec->icon, FAP_MANIFEST_MAX_ICON_SIZE);
            }
            furi_string_set_strn(
                item_name, rec->name, strnlen(rec->name, FAP_MANIFEST_MAX_APP_NAME_LENGTH));
            hit = true;
        }
    }
    furi_mutex_release(fap_icon_cache.mutex);
    return hit;
}

static void fap_icon_cache_put(
    Storage* storage,
    const char* path,
    uint32_t size,
    bool has_icon,
    FuriString* name,
    const void* icon) {
    fap_icon_cache_ensure_mutex();
    if(!fap_icon_cache.mutex) return;

    furi_mutex_acquire(fap_icon_cache.mutex, FuriWaitForever);
    fap_icon_cache_load_locked(storage);
    if(!fap_icon_cache.disabled) {
        FapIconCacheRecord rec;
        memset(&rec, 0, sizeof(rec));
        /* Caller guarantees strlen(path) < FAP_ICON_CACHE_PATH_MAX, so the
         * memset'd tail leaves a null terminator. memcpy avoids the
         * -Wstringop-truncation warnings strncpy would raise. */
        memcpy(rec.path, path, strlen(path));
        rec.size = size;
        rec.has_icon = has_icon ? 1 : 0;
        size_t nlen = strnlen(furi_string_get_cstr(name), FAP_MANIFEST_MAX_APP_NAME_LENGTH);
        memcpy(rec.name, furi_string_get_cstr(name), nlen);
        if(has_icon && icon) memcpy(rec.icon, icon, FAP_MANIFEST_MAX_ICON_SIZE);

        bool existed = fap_icon_cache_upsert_ram_locked(&rec);
        if(!fap_icon_cache.disabled) {
            if(existed) {
                fap_icon_cache_rewrite_file_locked(storage); /* update -> compact rewrite (rare) */
            } else {
                fap_icon_cache_append_file_locked(storage, &rec); /* new -> O(1) append */
            }
        }
    }
    furi_mutex_release(fap_icon_cache.mutex);
}

bool flipper_application_load_name_and_icon(
    FuriString* path,
    Storage* storage,
    uint8_t** icon_ptr,
    FuriString* item_name) {
    furi_check(path);
    furi_check(storage);
    furi_check(icon_ptr);
    furi_check(item_name);

    const char* path_cstr = furi_string_get_cstr(path);

    /* File size is the cache key. Paths that don't fit the record or files we
     * can't stat simply bypass the cache and get parsed directly. */
    uint32_t file_size = 0;
    bool cacheable = strlen(path_cstr) < FAP_ICON_CACHE_PATH_MAX;
    if(cacheable) {
        FileInfo info;
        if(storage_common_stat(storage, path_cstr, &info) == FSE_OK) {
            file_size = (uint32_t)info.size;
        } else {
            cacheable = false;
        }
    }

    if(cacheable && fap_icon_cache_try_get(storage, path_cstr, file_size, icon_ptr, item_name)) {
        return true;
    }

    /* Cache miss -> the expensive path: open the FAP and parse its manifest. */
    FlipperApplication* app = flipper_application_alloc(storage, NULL);
    if(!app) {
        return false;
    }

    FlipperApplicationPreloadStatus preload_res =
        flipper_application_preload_manifest(app, path_cstr);

    bool load_success = false;

    if(preload_res == FlipperApplicationPreloadStatusSuccess) {
        const FlipperApplicationManifest* manifest = flipper_application_get_manifest(app);
        bool has_icon = manifest->has_icon;
        if(has_icon && *icon_ptr) {
            memcpy(*icon_ptr, manifest->icon, FAP_MANIFEST_MAX_ICON_SIZE);
        }
        furi_string_set_strn(
            item_name, manifest->name, strnlen(manifest->name, FAP_MANIFEST_MAX_APP_NAME_LENGTH));
        load_success = true;

        if(cacheable) {
            fap_icon_cache_put(
                storage,
                path_cstr,
                file_size,
                has_icon,
                item_name,
                has_icon ? manifest->icon : NULL);
        }
    } else {
        FURI_LOG_W(
            TAG,
            "Metadata preload failed for %s: %s",
            path_cstr,
            flipper_application_preload_status_to_string(preload_res));
    }

    flipper_application_free(app);
    return load_success;
}
