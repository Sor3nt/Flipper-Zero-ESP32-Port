#include "flipper_application.h"

#include "elf/elf_file.h"

#include <toolbox/path.h>

#include <string.h>

#define TAG "FlipperApplication"

struct FlipperApplication {
    Storage* storage;
    FlipperApplicationManifest manifest;
    ELFFile* elf;
};

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
    flipper_application_load_manifest_data(FlipperApplication* app, const char* path) {
    furi_check(app);
    furi_check(path);

    if(!elf_file_open(app->elf, path)) {
        return FlipperApplicationPreloadStatusInvalidFile;
    }

    if(elf_process_section(
           app->elf, ".fapmeta", flipper_application_process_manifest_section, &app->manifest) !=
       ElfProcessSectionResultSuccess) {
        return FlipperApplicationPreloadStatusInvalidFile;
    }

    if(!flipper_application_manifest_is_valid(&app->manifest)) {
        return FlipperApplicationPreloadStatusInvalidManifest;
    }

    return FlipperApplicationPreloadStatusSuccess;
}

FlipperApplication*
    flipper_application_alloc(Storage* storage, const ElfApiInterface* api_interface) {
    furi_check(storage);

    FlipperApplication* app = calloc(1, sizeof(FlipperApplication));
    furi_check(app);

    app->storage = storage;
    app->elf = elf_file_alloc(storage, api_interface);
    if(!app->elf) {
        free(app);
        return NULL;
    }

    return app;
}

void flipper_application_free(FlipperApplication* app) {
    if(!app) return;

    elf_file_free(app->elf);
    free(app);
}

FlipperApplicationPreloadStatus
    flipper_application_preload_manifest(FlipperApplication* app, const char* path) {
    furi_check(app);
    furi_check(path);

    FlipperApplicationPreloadStatus status = flipper_application_load_manifest_data(app, path);
    if(status != FlipperApplicationPreloadStatusSuccess) {
        return status;
    }

    return flipper_application_validate_manifest(app);
}

FlipperApplicationPreloadStatus flipper_application_preload(FlipperApplication* app, const char* path) {
    furi_check(app);
    furi_check(path);

    return flipper_application_preload_manifest(app, path);
}

const FlipperApplicationManifest* flipper_application_get_manifest(FlipperApplication* app) {
    furi_check(app);

    return &app->manifest;
}

FlipperApplicationLoadStatus flipper_application_map_to_memory(FlipperApplication* app) {
    furi_check(app);

    return FlipperApplicationLoadStatusUnspecifiedError;
}

FuriThread* flipper_application_alloc_thread(FlipperApplication* app, const char* args) {
    furi_check(app);
    UNUSED(args);

    return NULL;
}

bool flipper_application_is_plugin(FlipperApplication* app) {
    furi_check(app);

    return false;
}

const FlipperAppPluginDescriptor*
    flipper_application_plugin_get_descriptor(FlipperApplication* app) {
    furi_check(app);

    return NULL;
}

const char* flipper_application_preload_status_to_string(FlipperApplicationPreloadStatus status) {
    switch(status) {
    case FlipperApplicationPreloadStatusSuccess:
        return "success";
    case FlipperApplicationPreloadStatusInvalidFile:
        return "invalid file";
    case FlipperApplicationPreloadStatusNotEnoughMemory:
        return "not enough memory";
    case FlipperApplicationPreloadStatusInvalidManifest:
        return "invalid manifest";
    case FlipperApplicationPreloadStatusApiTooOld:
        return "app is too old";
    case FlipperApplicationPreloadStatusApiTooNew:
        return "app is too new";
    case FlipperApplicationPreloadStatusTargetMismatch:
        return "hardware target mismatch";
    default:
        return "unknown preload status";
    }
}

const char* flipper_application_load_status_to_string(FlipperApplicationLoadStatus status) {
    switch(status) {
    case FlipperApplicationLoadStatusSuccess:
        return "success";
    case FlipperApplicationLoadStatusUnspecifiedError:
        return "unspecified error";
    case FlipperApplicationLoadStatusMissingImports:
        return "missing imports";
    default:
        return "unknown load status";
    }
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

    FlipperApplication* app = flipper_application_alloc(storage, NULL);
    if(!app) {
        return false;
    }

    FlipperApplicationPreloadStatus preload_res =
        flipper_application_load_manifest_data(app, furi_string_get_cstr(path));

    bool load_success = false;

    if(preload_res == FlipperApplicationPreloadStatusSuccess) {
        const FlipperApplicationManifest* manifest = flipper_application_get_manifest(app);
        if(manifest->has_icon && *icon_ptr) {
            memcpy(*icon_ptr, manifest->icon, FAP_MANIFEST_MAX_ICON_SIZE);
        }
        furi_string_set_strn(
            item_name, manifest->name, strnlen(manifest->name, FAP_MANIFEST_MAX_APP_NAME_LENGTH));
        load_success = true;
    } else {
        FURI_LOG_W(
            TAG,
            "Metadata preload failed for %s: %s",
            furi_string_get_cstr(path),
            flipper_application_preload_status_to_string(preload_res));
    }

    flipper_application_free(app);
    return load_success;
}
