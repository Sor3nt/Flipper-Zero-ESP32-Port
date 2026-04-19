/**
 * @file flipper_application.h
 * Flipper application
 */
#pragma once

#include "application_manifest.h"
#include "elf/elf_api_interface.h"

#include <furi.h>
#include <storage/storage.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FlipperApplicationPreloadStatusSuccess = 0,
    FlipperApplicationPreloadStatusInvalidFile,
    FlipperApplicationPreloadStatusNotEnoughMemory,
    FlipperApplicationPreloadStatusInvalidManifest,
    FlipperApplicationPreloadStatusApiTooOld,
    FlipperApplicationPreloadStatusApiTooNew,
    FlipperApplicationPreloadStatusTargetMismatch,
} FlipperApplicationPreloadStatus;

typedef enum {
    FlipperApplicationLoadStatusSuccess = 0,
    FlipperApplicationLoadStatusUnspecifiedError,
    FlipperApplicationLoadStatusMissingImports,
} FlipperApplicationLoadStatus;

const char* flipper_application_preload_status_to_string(FlipperApplicationPreloadStatus status);
const char* flipper_application_load_status_to_string(FlipperApplicationLoadStatus status);

typedef struct FlipperApplication FlipperApplication;

typedef struct {
    const char* name;
    uint32_t address;
} FlipperApplicationMemoryMapEntry;

typedef struct {
    uint32_t mmap_entry_count;
    FlipperApplicationMemoryMapEntry* mmap_entries;
    uint32_t debug_link_size;
    uint8_t* debug_link;
} FlipperApplicationState;

FlipperApplication*
    flipper_application_alloc(Storage* storage, const ElfApiInterface* api_interface);
void flipper_application_free(FlipperApplication* app);

FlipperApplicationPreloadStatus
    flipper_application_preload(FlipperApplication* app, const char* path);
FlipperApplicationPreloadStatus
    flipper_application_preload_manifest(FlipperApplication* app, const char* path);
const FlipperApplicationManifest* flipper_application_get_manifest(FlipperApplication* app);
FlipperApplicationLoadStatus flipper_application_map_to_memory(FlipperApplication* app);
FuriThread* flipper_application_alloc_thread(FlipperApplication* app, const char* args);
bool flipper_application_is_plugin(FlipperApplication* app);

typedef int32_t (*FlipperApplicationEntryPoint)(void*);

typedef struct {
    const char* appid;
    const uint32_t ep_api_version;
    const void* entry_point;
} FlipperAppPluginDescriptor;

typedef const FlipperAppPluginDescriptor* (*FlipperApplicationPluginEntryPoint)(void);

const FlipperAppPluginDescriptor*
    flipper_application_plugin_get_descriptor(FlipperApplication* app);

bool flipper_application_load_name_and_icon(
    FuriString* path,
    Storage* storage,
    uint8_t** icon_ptr,
    FuriString* item_name);

#ifdef __cplusplus
}
#endif
