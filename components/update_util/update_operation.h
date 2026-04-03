#pragma once

#include <furi.h>
#include <storage/storage.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UPDATE_OPERATION_ROOT_DIR_PACKAGE_MAGIC 0
#define UPDATE_OPERATION_MAX_MANIFEST_PATH_LEN  255u
#define UPDATE_OPERATION_MIN_MANIFEST_VERSION   2

typedef enum {
    UpdatePrepareResultOK,
    UpdatePrepareResultManifestPathInvalid,
    UpdatePrepareResultManifestFolderNotFound,
    UpdatePrepareResultManifestInvalid,
    UpdatePrepareResultStageMissing,
    UpdatePrepareResultStageIntegrityError,
    UpdatePrepareResultManifestPointerCreateError,
    UpdatePrepareResultManifestPointerCheckError,
    UpdatePrepareResultTargetMismatch,
    UpdatePrepareResultOutdatedManifestVersion,
    UpdatePrepareResultIntFull,
    UpdatePrepareResultUnspecifiedError,
} UpdatePrepareResult;

static inline bool update_operation_get_package_dir_name(
    const char* full_path,
    FuriString* out_manifest_dir) {
    UNUSED(full_path);
    if(out_manifest_dir) {
        furi_string_reset(out_manifest_dir);
    }
    return false;
}

static inline const char*
    update_operation_describe_preparation_result(const UpdatePrepareResult value) {
    UNUSED(value);
    return "Not supported on ESP";
}

static inline UpdatePrepareResult update_operation_prepare(const char* manifest_file_path) {
    UNUSED(manifest_file_path);
    return UpdatePrepareResultUnspecifiedError;
}

static inline bool update_operation_get_current_package_manifest_path(
    Storage* storage,
    FuriString* out_path) {
    UNUSED(storage);
    if(out_path) {
        furi_string_reset(out_path);
    }
    return false;
}

static inline bool update_operation_is_armed(void) {
    return false;
}

static inline void update_operation_disarm(void) {
}

#ifdef __cplusplus
}
#endif
