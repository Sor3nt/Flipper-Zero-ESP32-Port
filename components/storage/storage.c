/**
 * @file storage.c
 * @brief Storage service for ESP32 — VFS-based implementation
 *
 * Maps Flipper /ext paths to /sdcard VFS mount point.
 * Uses POSIX file ops (fopen, fread, etc.) on the ESP-IDF VFS layer.
 * Thread-safe via mutex (no message queue IPC like STM32).
 */

#include "storage.h"
#include "storage_i.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include <furi.h>
#include <furi_hal_sd.h>
#include <furi_hal_spi_bus.h>

#include <esp_log.h>

static const char* TAG = "Storage";

#define SD_MOUNT_POINT "/sdcard"
#define COPY_BUF_SIZE  512

static inline void storage_sd_bus_lock(void);
static inline void storage_sd_bus_unlock(void);

static const char* storage_current_appid(void) {
    FuriThreadId thread_id = furi_thread_get_current_id();
    const char* appid = thread_id ? furi_thread_get_appid(thread_id) : NULL;
    if(!appid || !appid[0]) {
        appid = "system";
    }
    return appid;
}

static bool storage_build_app_alias_path(
    const char* path,
    const char* alias_prefix,
    const char* real_root,
    char* out,
    size_t out_size) {
    const size_t prefix_len = strlen(alias_prefix);
    if(strncmp(path, alias_prefix, prefix_len) != 0) {
        return false;
    }

    const char* suffix = path + prefix_len;
    int written = snprintf(
        out,
        out_size,
        "%s/%s/%s%s",
        SD_MOUNT_POINT,
        real_root,
        storage_current_appid(),
        suffix);
    return written > 0 && (size_t)written < out_size;
}

static void storage_ensure_app_alias_dir(Storage* storage, const char* real_root) {
    UNUSED(storage);
    char base_path[256];
    char app_path[256];

    snprintf(base_path, sizeof(base_path), "%s/%s", SD_MOUNT_POINT, real_root);
    snprintf(app_path, sizeof(app_path), "%s/%s/%s", SD_MOUNT_POINT, real_root, storage_current_appid());

    storage_sd_bus_lock();
    mkdir(base_path, 0755);
    mkdir(app_path, 0755);
    storage_sd_bus_unlock();
}

static inline void storage_sd_bus_lock(void) {
    furi_hal_spi_bus_lock();
}

static inline void storage_sd_bus_unlock(void) {
    furi_hal_spi_bus_unlock();
}

/* ---- Internal File struct ---- */

struct File {
    Storage* storage;
    FILE* handle;
    DIR* dir_handle;
    char* dir_path;
    FS_Error error_id;
    int32_t internal_error;
    bool is_open;
    bool is_dir;
};

/* ---- Path mapping: /ext/foo -> /sdcard/foo ---- */

static bool storage_map_path(const char* path, char* out, size_t out_size) {
    if(!path || !out) return false;

    if(strncmp(path, STORAGE_EXT_PATH_PREFIX, 4) == 0) {
        snprintf(out, out_size, "%s%s", SD_MOUNT_POINT, path + 4);
        return true;
    }
    if(strncmp(path, STORAGE_ANY_PATH_PREFIX, 4) == 0) {
        snprintf(out, out_size, "%s%s", SD_MOUNT_POINT, path + 4);
        return true;
    }
    if(strncmp(path, STORAGE_INT_PATH_PREFIX, 4) == 0) {
        /* Internal storage — map to /sdcard/.int for now */
        snprintf(out, out_size, "%s/.int%s", SD_MOUNT_POINT, path + 4);
        return true;
    }
    if(storage_build_app_alias_path(
           path, STORAGE_APP_DATA_PATH_PREFIX, "apps_data", out, out_size)) {
        return true;
    }
    if(storage_build_app_alias_path(
           path, STORAGE_APP_ASSETS_PATH_PREFIX, "apps_assets", out, out_size)) {
        return true;
    }

    /* Passthrough for absolute paths */
    snprintf(out, out_size, "%s", path);
    return true;
}

/* ---- Error helpers ---- */

const char* filesystem_api_error_get_desc(FS_Error error_id) {
    switch(error_id) {
    case FSE_OK:
        return "OK";
    case FSE_NOT_READY:
        return "Not ready";
    case FSE_EXIST:
        return "Already exists";
    case FSE_NOT_EXIST:
        return "Does not exist";
    case FSE_INVALID_PARAMETER:
        return "Invalid parameter";
    case FSE_DENIED:
        return "Access denied";
    case FSE_INVALID_NAME:
        return "Invalid name";
    case FSE_INTERNAL:
        return "Internal error";
    case FSE_NOT_IMPLEMENTED:
        return "Not implemented";
    case FSE_ALREADY_OPEN:
        return "Already open";
    default:
        return "Unknown error";
    }
}

bool file_info_is_dir(const FileInfo* file_info) {
    furi_assert(file_info);
    return (file_info->flags & FSF_DIRECTORY) != 0;
}

const char* storage_error_get_desc(FS_Error error_id) {
    return filesystem_api_error_get_desc(error_id);
}

FS_Error storage_file_get_error(File* file) {
    furi_assert(file);
    return file->error_id;
}

int32_t storage_file_get_internal_error(File* file) {
    furi_assert(file);
    return file->internal_error;
}

const char* storage_file_get_error_desc(File* file) {
    furi_assert(file);
    return filesystem_api_error_get_desc(file->error_id);
}

const char* sd_api_get_fs_type_text(SDFsType fs_type) {
    switch(fs_type) {
    case FST_FAT12:
        return "FAT12";
    case FST_FAT16:
        return "FAT16";
    case FST_FAT32:
        return "FAT32";
    case FST_EXFAT:
        return "exFAT";
    default:
        return "UNKNOWN";
    }
}

static FS_Error storage_errno_to_fserror(int err) {
    switch(err) {
    case 0:
        return FSE_OK;
    case ENOENT:
        return FSE_NOT_EXIST;
    case EEXIST:
        return FSE_EXIST;
    case EACCES:
    case EPERM:
        return FSE_DENIED;
    case EINVAL:
    case ENAMETOOLONG:
        return FSE_INVALID_NAME;
    default:
        return FSE_INTERNAL;
    }
}

/* ---- File alloc/free ---- */

File* storage_file_alloc(Storage* storage) {
    furi_assert(storage);
    File* file = malloc(sizeof(File));
    memset(file, 0, sizeof(File));
    file->storage = storage;
    file->error_id = FSE_OK;
    return file;
}

void storage_file_free(File* file) {
    if(!file) return;
    if(file->is_open) {
        if(file->is_dir) {
            storage_dir_close(file);
        } else {
            storage_file_close(file);
        }
    }
    if(file->dir_path) {
        free(file->dir_path);
    }
    free(file);
}

/* ---- File operations ---- */

bool storage_file_open(
    File* file,
    const char* path,
    FS_AccessMode access_mode,
    FS_OpenMode open_mode) {
    furi_assert(file);
    furi_assert(path);

    if(file->is_open) {
        file->error_id = FSE_ALREADY_OPEN;
        return false;
    }

    if(!file->storage->sd_mounted) {
        file->error_id = FSE_NOT_READY;
        return false;
    }

    char real_path[256];
    if(!storage_map_path(path, real_path, sizeof(real_path))) {
        file->error_id = FSE_INVALID_NAME;
        return false;
    }

    if(strncmp(path, STORAGE_APP_DATA_PATH_PREFIX, strlen(STORAGE_APP_DATA_PATH_PREFIX)) == 0) {
        storage_ensure_app_alias_dir(file->storage, "apps_data");
    } else if(
        strncmp(path, STORAGE_APP_ASSETS_PATH_PREFIX, strlen(STORAGE_APP_ASSETS_PATH_PREFIX)) ==
        0) {
        storage_ensure_app_alias_dir(file->storage, "apps_assets");
    }

    /* Build fopen mode string */
    const char* mode = "r";
    switch(open_mode) {
    case FSOM_OPEN_EXISTING:
        mode = (access_mode & FSAM_WRITE) ? "r+b" : "rb";
        break;
    case FSOM_OPEN_ALWAYS:
        /* Open if exists, create if not. Use "r+" first, fallback to "w+" */
        if(access_mode & FSAM_WRITE) {
            file->handle = fopen(real_path, "r+b");
            if(file->handle) {
                file->is_open = true;
                file->is_dir = false;
                file->error_id = FSE_OK;
                return true;
            }
            mode = "w+b";
        } else {
            mode = "rb";
        }
        break;
    case FSOM_OPEN_APPEND:
        mode = "ab";
        break;
    case FSOM_CREATE_NEW:
        /* Fail if exists */
        {
            struct stat st;
            if(stat(real_path, &st) == 0) {
                file->error_id = FSE_EXIST;
                return false;
            }
            mode = "w+b";
        }
        break;
    case FSOM_CREATE_ALWAYS:
        mode = "w+b";
        break;
    default:
        file->error_id = FSE_INVALID_PARAMETER;
        return false;
    }

    furi_mutex_acquire(file->storage->mutex, FuriWaitForever);
    storage_sd_bus_lock();
    file->handle = fopen(real_path, mode);
    storage_sd_bus_unlock();
    furi_mutex_release(file->storage->mutex);

    if(!file->handle) {
        file->error_id = storage_errno_to_fserror(errno);
        file->internal_error = errno;
        return false;
    }

    file->is_open = true;
    file->is_dir = false;
    file->error_id = FSE_OK;
    return true;
}

bool storage_file_close(File* file) {
    furi_assert(file);
    if(!file->is_open || file->is_dir) return false;

    if(file->handle) {
        storage_sd_bus_lock();
        fclose(file->handle);
        storage_sd_bus_unlock();
        file->handle = NULL;
    }
    file->is_open = false;
    file->error_id = FSE_OK;

    /* Publish close event */
    StorageEvent event = {.type = StorageEventTypeFileClose};
    furi_pubsub_publish(file->storage->pubsub, &event);

    return true;
}

bool storage_file_is_open(File* file) {
    furi_assert(file);
    return file->is_open && !file->is_dir;
}

bool storage_file_is_dir(File* file) {
    furi_assert(file);
    return file->is_open && file->is_dir;
}

size_t storage_file_read(File* file, void* buff, size_t bytes_to_read) {
    furi_assert(file);
    furi_assert(buff);
    if(!file->is_open || !file->handle || file->is_dir) {
        file->error_id = FSE_INTERNAL;
        return 0;
    }

    storage_sd_bus_lock();
    size_t read = fread(buff, 1, bytes_to_read, file->handle);
    storage_sd_bus_unlock();
    if(read < bytes_to_read && ferror(file->handle)) {
        file->error_id = FSE_INTERNAL;
        file->internal_error = errno;
    } else {
        file->error_id = FSE_OK;
    }
    return read;
}

size_t storage_file_write(File* file, const void* buff, size_t bytes_to_write) {
    furi_assert(file);
    furi_assert(buff);
    if(!file->is_open || !file->handle || file->is_dir) {
        file->error_id = FSE_INTERNAL;
        return 0;
    }

    storage_sd_bus_lock();
    size_t written = fwrite(buff, 1, bytes_to_write, file->handle);
    storage_sd_bus_unlock();
    if(written < bytes_to_write) {
        file->error_id = FSE_INTERNAL;
        file->internal_error = errno;
    } else {
        file->error_id = FSE_OK;
    }
    return written;
}

bool storage_file_seek(File* file, uint32_t offset, bool from_start) {
    furi_assert(file);
    if(!file->is_open || !file->handle || file->is_dir) {
        file->error_id = FSE_INTERNAL;
        return false;
    }

    int whence = from_start ? SEEK_SET : SEEK_CUR;
    storage_sd_bus_lock();
    int ret = fseek(file->handle, (long)offset, whence);
    storage_sd_bus_unlock();
    if(ret != 0) {
        file->error_id = FSE_INTERNAL;
        file->internal_error = errno;
        return false;
    }
    file->error_id = FSE_OK;
    return true;
}

uint64_t storage_file_tell(File* file) {
    furi_assert(file);
    if(!file->is_open || !file->handle || file->is_dir) return 0;

    storage_sd_bus_lock();
    long pos = ftell(file->handle);
    storage_sd_bus_unlock();
    if(pos < 0) {
        file->error_id = FSE_INTERNAL;
        return 0;
    }
    file->error_id = FSE_OK;
    return (uint64_t)pos;
}

bool storage_file_truncate(File* file) {
    furi_assert(file);
    if(!file->is_open || !file->handle || file->is_dir) {
        file->error_id = FSE_INTERNAL;
        return false;
    }

    storage_sd_bus_lock();
    long pos = ftell(file->handle);
    if(pos < 0) {
        storage_sd_bus_unlock();
        file->error_id = FSE_INTERNAL;
        return false;
    }

    int fd = fileno(file->handle);
    if(ftruncate(fd, pos) != 0) {
        storage_sd_bus_unlock();
        file->error_id = FSE_INTERNAL;
        file->internal_error = errno;
        return false;
    }
    storage_sd_bus_unlock();
    file->error_id = FSE_OK;
    return true;
}

uint64_t storage_file_size(File* file) {
    furi_assert(file);
    if(!file->is_open || !file->handle || file->is_dir) return 0;

    storage_sd_bus_lock();
    long cur = ftell(file->handle);
    fseek(file->handle, 0, SEEK_END);
    long size = ftell(file->handle);
    fseek(file->handle, cur, SEEK_SET);
    storage_sd_bus_unlock();

    if(size < 0) {
        file->error_id = FSE_INTERNAL;
        return 0;
    }
    file->error_id = FSE_OK;
    return (uint64_t)size;
}

bool storage_file_sync(File* file) {
    furi_assert(file);
    if(!file->is_open || !file->handle || file->is_dir) {
        file->error_id = FSE_INTERNAL;
        return false;
    }

    storage_sd_bus_lock();
    fflush(file->handle);
    int fd = fileno(file->handle);
    fsync(fd);
    storage_sd_bus_unlock();
    file->error_id = FSE_OK;
    return true;
}

bool storage_file_eof(File* file) {
    furi_assert(file);
    if(!file->is_open || !file->handle || file->is_dir) return true;
    return feof(file->handle) != 0;
}

bool storage_file_exists(Storage* storage, const char* path) {
    furi_assert(storage);
    furi_assert(path);

    char real_path[256];
    if(!storage_map_path(path, real_path, sizeof(real_path))) return false;

    struct stat st;
    storage_sd_bus_lock();
    bool exists = stat(real_path, &st) == 0 && !(st.st_mode & S_IFDIR);
    storage_sd_bus_unlock();
    return exists;
}

bool storage_file_copy_to_file(File* source, File* destination, size_t size) {
    furi_assert(source);
    furi_assert(destination);
    if(!source->is_open || !destination->is_open) return false;

    uint8_t buf[COPY_BUF_SIZE];
    size_t remaining = size;

    while(remaining > 0) {
        size_t to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
        size_t read = storage_file_read(source, buf, to_read);
        if(read == 0) break;
        size_t written = storage_file_write(destination, buf, read);
        if(written != read) return false;
        remaining -= read;
    }

    return remaining == 0;
}

/* ---- Directory operations ---- */

bool storage_dir_open(File* file, const char* path) {
    furi_assert(file);
    furi_assert(path);

    if(file->is_open) {
        file->error_id = FSE_ALREADY_OPEN;
        return false;
    }

    if(!file->storage->sd_mounted) {
        file->error_id = FSE_NOT_READY;
        return false;
    }

    char real_path[256];
    if(!storage_map_path(path, real_path, sizeof(real_path))) {
        file->error_id = FSE_INVALID_NAME;
        return false;
    }

    storage_sd_bus_lock();
    file->dir_handle = opendir(real_path);
    storage_sd_bus_unlock();
    if(!file->dir_handle) {
        file->error_id = storage_errno_to_fserror(errno);
        file->internal_error = errno;
        return false;
    }

    file->dir_path = strdup(real_path);
    file->is_open = true;
    file->is_dir = true;
    file->error_id = FSE_OK;
    return true;
}

bool storage_dir_close(File* file) {
    furi_assert(file);
    if(!file->is_open || !file->is_dir) return false;

    if(file->dir_handle) {
        storage_sd_bus_lock();
        closedir(file->dir_handle);
        storage_sd_bus_unlock();
        file->dir_handle = NULL;
    }
    if(file->dir_path) {
        free(file->dir_path);
        file->dir_path = NULL;
    }
    file->is_open = false;
    file->error_id = FSE_OK;

    StorageEvent event = {.type = StorageEventTypeDirClose};
    furi_pubsub_publish(file->storage->pubsub, &event);

    return true;
}

bool storage_dir_read(File* file, FileInfo* fileinfo, char* name, uint16_t name_length) {
    furi_assert(file);
    if(!file->is_open || !file->is_dir || !file->dir_handle) {
        file->error_id = FSE_INTERNAL;
        return false;
    }

    storage_sd_bus_lock();
    struct dirent* entry = readdir(file->dir_handle);
    if(!entry) {
        storage_sd_bus_unlock();
        file->error_id = FSE_NOT_EXIST;
        return false;
    }

    if(name && name_length > 0) {
        strncpy(name, entry->d_name, name_length - 1);
        name[name_length - 1] = '\0';
    }

    if(fileinfo) {
        memset(fileinfo, 0, sizeof(FileInfo));

        if(entry->d_type == DT_DIR) {
            fileinfo->flags = FSF_DIRECTORY;
        } else if(file->dir_path) {
            /* Get file size via stat */
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s/%s", file->dir_path, entry->d_name);
            struct stat st;
            if(stat(full_path, &st) == 0) {
                fileinfo->size = st.st_size;
            }
        }
    }
    storage_sd_bus_unlock();

    file->error_id = FSE_OK;
    return true;
}

bool storage_dir_rewind(File* file) {
    furi_assert(file);
    if(!file->is_open || !file->is_dir || !file->dir_handle) {
        file->error_id = FSE_INTERNAL;
        return false;
    }

    storage_sd_bus_lock();
    rewinddir(file->dir_handle);
    storage_sd_bus_unlock();
    file->error_id = FSE_OK;
    return true;
}

bool storage_dir_exists(Storage* storage, const char* path) {
    furi_assert(storage);
    furi_assert(path);

    char real_path[256];
    if(!storage_map_path(path, real_path, sizeof(real_path))) return false;

    struct stat st;
    storage_sd_bus_lock();
    bool exists = stat(real_path, &st) == 0 && (st.st_mode & S_IFDIR);
    storage_sd_bus_unlock();
    return exists;
}

/* ---- Common operations ---- */

FS_Error storage_common_timestamp(Storage* storage, const char* path, uint32_t* timestamp) {
    furi_assert(storage);
    furi_assert(path);

    char real_path[256];
    if(!storage_map_path(path, real_path, sizeof(real_path))) return FSE_INVALID_NAME;

    struct stat st;
    storage_sd_bus_lock();
    if(stat(real_path, &st) != 0) {
        storage_sd_bus_unlock();
        return storage_errno_to_fserror(errno);
    }
    storage_sd_bus_unlock();

    if(timestamp) *timestamp = st.st_mtime;
    return FSE_OK;
}

FS_Error storage_common_stat(Storage* storage, const char* path, FileInfo* fileinfo) {
    furi_assert(storage);
    furi_assert(path);

    char real_path[256];
    if(!storage_map_path(path, real_path, sizeof(real_path))) return FSE_INVALID_NAME;

    struct stat st;
    storage_sd_bus_lock();
    if(stat(real_path, &st) != 0) {
        storage_sd_bus_unlock();
        return storage_errno_to_fserror(errno);
    }
    storage_sd_bus_unlock();

    if(fileinfo) {
        memset(fileinfo, 0, sizeof(FileInfo));
        if(S_ISDIR(st.st_mode)) {
            fileinfo->flags = FSF_DIRECTORY;
        }
        fileinfo->size = st.st_size;
    }

    return FSE_OK;
}

FS_Error storage_common_remove(Storage* storage, const char* path) {
    furi_assert(storage);
    furi_assert(path);

    char real_path[256];
    if(!storage_map_path(path, real_path, sizeof(real_path))) return FSE_INVALID_NAME;

    struct stat st;
    storage_sd_bus_lock();
    if(stat(real_path, &st) != 0) {
        storage_sd_bus_unlock();
        return FSE_NOT_EXIST;
    }

    int ret;
    if(S_ISDIR(st.st_mode)) {
        ret = rmdir(real_path);
    } else {
        ret = unlink(real_path);
    }
    storage_sd_bus_unlock();

    if(ret != 0) {
        return storage_errno_to_fserror(errno);
    }
    return FSE_OK;
}

FS_Error storage_common_rename(Storage* storage, const char* old_path, const char* new_path) {
    furi_assert(storage);

    char real_old[256], real_new[256];
    if(!storage_map_path(old_path, real_old, sizeof(real_old))) return FSE_INVALID_NAME;
    if(!storage_map_path(new_path, real_new, sizeof(real_new))) return FSE_INVALID_NAME;

    storage_sd_bus_lock();
    int rename_result = rename(real_old, real_new);
    storage_sd_bus_unlock();
    if(rename_result != 0) {
        return storage_errno_to_fserror(errno);
    }
    return FSE_OK;
}

FS_Error storage_common_copy(Storage* storage, const char* old_path, const char* new_path) {
    furi_assert(storage);

    File* src = storage_file_alloc(storage);
    File* dst = storage_file_alloc(storage);
    FS_Error result = FSE_INTERNAL;

    do {
        if(!storage_file_open(src, old_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
            result = src->error_id;
            break;
        }
        if(!storage_file_open(dst, new_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            result = dst->error_id;
            break;
        }

        uint8_t buf[COPY_BUF_SIZE];
        size_t read;
        result = FSE_OK;
        while((read = storage_file_read(src, buf, sizeof(buf))) > 0) {
            size_t written = storage_file_write(dst, buf, read);
            if(written != read) {
                result = FSE_INTERNAL;
                break;
            }
        }
    } while(false);

    storage_file_free(src);
    storage_file_free(dst);
    return result;
}

FS_Error storage_common_mkdir(Storage* storage, const char* path) {
    furi_assert(storage);
    furi_assert(path);

    char real_path[256];
    if(!storage_map_path(path, real_path, sizeof(real_path))) return FSE_INVALID_NAME;

    storage_sd_bus_lock();
    int mkdir_result = mkdir(real_path, 0755);
    storage_sd_bus_unlock();
    if(mkdir_result != 0) {
        if(errno == EEXIST) return FSE_EXIST;
        return storage_errno_to_fserror(errno);
    }
    return FSE_OK;
}

void storage_common_resolve_path_and_ensure_app_directory(Storage* storage, FuriString* path) {
    furi_assert(storage);
    furi_assert(path);

    char resolved_path[256];

    if(storage_build_app_alias_path(
           furi_string_get_cstr(path),
           STORAGE_APP_DATA_PATH_PREFIX,
           "apps_data",
           resolved_path,
           sizeof(resolved_path))) {
        storage_ensure_app_alias_dir(storage, "apps_data");
        furi_string_printf(path, "%s/apps_data/%s%s", STORAGE_EXT_PATH_PREFIX, storage_current_appid(), furi_string_get_cstr(path) + strlen(STORAGE_APP_DATA_PATH_PREFIX));
        return;
    }

    if(storage_build_app_alias_path(
           furi_string_get_cstr(path),
           STORAGE_APP_ASSETS_PATH_PREFIX,
           "apps_assets",
           resolved_path,
           sizeof(resolved_path))) {
        storage_ensure_app_alias_dir(storage, "apps_assets");
        furi_string_printf(path, "%s/apps_assets/%s%s", STORAGE_EXT_PATH_PREFIX, storage_current_appid(), furi_string_get_cstr(path) + strlen(STORAGE_APP_ASSETS_PATH_PREFIX));
    }
}

FS_Error storage_common_fs_info(
    Storage* storage,
    const char* fs_path,
    uint64_t* total_space,
    uint64_t* free_space) {
    furi_assert(storage);
    (void)fs_path;

    if(!storage->sd_mounted) return FSE_NOT_READY;

    FuriHalSdInfo info;
    if(furi_hal_sd_info(&info) != FuriStatusOk) return FSE_INTERNAL;

    if(total_space) *total_space = info.capacity;
    /* Free space not easily available without FATFS API — estimate */
    if(free_space) *free_space = 0;

    return FSE_OK;
}

bool storage_common_exists(Storage* storage, const char* path) {
    furi_assert(storage);
    furi_assert(path);

    char real_path[256];
    if(!storage_map_path(path, real_path, sizeof(real_path))) return false;

    struct stat st;
    storage_sd_bus_lock();
    bool exists = stat(real_path, &st) == 0;
    storage_sd_bus_unlock();
    return exists;
}

/* ---- SD Card functions ---- */

FS_Error storage_sd_format(Storage* storage) {
    (void)storage;
    return FSE_NOT_IMPLEMENTED;
}

FS_Error storage_sd_unmount(Storage* storage) {
    furi_assert(storage);
    if(!storage->sd_mounted) return FSE_NOT_READY;

    if(!furi_hal_sd_unmount()) return FSE_INTERNAL;

    storage->sd_mounted = false;
    StorageEvent event = {.type = StorageEventTypeCardUnmount};
    furi_pubsub_publish(storage->pubsub, &event);

    return FSE_OK;
}

FS_Error storage_sd_mount(Storage* storage) {
    furi_assert(storage);
    if(storage->sd_mounted) return FSE_OK;

    if(!furi_hal_sd_mount()) {
        StorageEvent event = {.type = StorageEventTypeCardMountError};
        furi_pubsub_publish(storage->pubsub, &event);
        return FSE_INTERNAL;
    }

    storage->sd_mounted = true;
    StorageEvent event = {.type = StorageEventTypeCardMount};
    furi_pubsub_publish(storage->pubsub, &event);

    return FSE_OK;
}

FS_Error storage_sd_info(Storage* storage, SDInfo* info) {
    furi_assert(storage);
    furi_assert(info);

    if(!storage->sd_mounted) return FSE_NOT_READY;

    memset(info, 0, sizeof(SDInfo));

    FuriHalSdInfo hal_info;
    if(furi_hal_sd_info(&hal_info) != FuriStatusOk) return FSE_INTERNAL;

    info->fs_type = FST_FAT32; /* Assume FAT32 for SD cards */
    info->kb_total = (uint32_t)(hal_info.capacity / 1024);
    info->kb_free = 0; /* Not easily available */
    info->sector_size = hal_info.sector_size;
    info->cluster_size = hal_info.sector_size; /* Approximation */

    info->manufacturer_id = hal_info.manufacturer_id;
    memcpy(info->oem_id, hal_info.oem_id, sizeof(info->oem_id));
    memcpy(info->product_name, hal_info.product_name, sizeof(info->product_name));
    info->product_revision_major = hal_info.product_revision_major;
    info->product_revision_minor = hal_info.product_revision_minor;
    info->product_serial_number = hal_info.product_serial_number;
    info->manufacturing_month = hal_info.manufacturing_month;
    info->manufacturing_year = hal_info.manufacturing_year;

    return FSE_OK;
}

FS_Error storage_sd_status(Storage* storage) {
    furi_assert(storage);
    if(storage->sd_mounted) return FSE_OK;
    return FSE_NOT_READY;
}

/* ---- Simplified functions ---- */

bool storage_simply_remove(Storage* storage, const char* path) {
    FS_Error err = storage_common_remove(storage, path);
    return err == FSE_OK || err == FSE_NOT_EXIST;
}

static bool storage_simply_remove_recursive_internal(Storage* storage, const char* real_path) {
    struct stat st;
    if(stat(real_path, &st) != 0) return true;

    if(!S_ISDIR(st.st_mode)) {
        return unlink(real_path) == 0;
    }

    DIR* dir = opendir(real_path);
    if(!dir) return false;

    bool success = true;
    struct dirent* entry;
    while((entry = readdir(dir)) != NULL) {
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char child[512];
        snprintf(child, sizeof(child), "%s/%s", real_path, entry->d_name);
        if(!storage_simply_remove_recursive_internal(storage, child)) {
            success = false;
        }
    }
    closedir(dir);

    if(success) {
        success = rmdir(real_path) == 0;
    }
    return success;
}

bool storage_simply_remove_recursive(Storage* storage, const char* path) {
    furi_assert(storage);
    furi_assert(path);

    char real_path[256];
    if(!storage_map_path(path, real_path, sizeof(real_path))) return false;

    return storage_simply_remove_recursive_internal(storage, real_path);
}

bool storage_simply_mkdir(Storage* storage, const char* path) {
    FS_Error err = storage_common_mkdir(storage, path);
    return err == FSE_OK || err == FSE_EXIST;
}

void storage_get_next_filename(
    Storage* storage,
    const char* dirname,
    const char* filename,
    const char* fileextension,
    FuriString* nextfilename,
    uint8_t max_len) {
    furi_assert(storage);
    furi_assert(nextfilename);

    FuriString* temp = furi_string_alloc();

    furi_string_printf(temp, "%s/%s%s", dirname, filename, fileextension);
    if(!storage_file_exists(storage, furi_string_get_cstr(temp))) {
        furi_string_set(nextfilename, filename);
        furi_string_free(temp);
        return;
    }

    for(uint16_t i = 1; i < 9999; i++) {
        furi_string_printf(temp, "%s/%s%u%s", dirname, filename, i, fileextension);
        if(!storage_file_exists(storage, furi_string_get_cstr(temp))) {
            furi_string_printf(nextfilename, "%s%u", filename, i);
            if(furi_string_size(nextfilename) > max_len) {
                furi_string_set(nextfilename, filename);
            }
            break;
        }
    }

    furi_string_free(temp);
}

/* ---- PubSub ---- */

FuriPubSub* storage_get_pubsub(Storage* storage) {
    furi_assert(storage);
    return storage->pubsub;
}

/* ---- Service entry point ---- */

int32_t storage_srv(void* p) {
    (void)p;
    FURI_LOG_I(TAG, "Starting Storage service");

    Storage* storage = malloc(sizeof(Storage));
    memset(storage, 0, sizeof(Storage));
    storage->pubsub = furi_pubsub_alloc();
    storage->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    /* Try to mount SD card */
    ESP_LOGI(TAG, "Attempting SD card mount...");
    if(furi_hal_sd_mount()) {
        storage->sd_mounted = true;
        ESP_LOGI(TAG, "SD card mounted successfully");

        /* Ensure the internal-storage shadow dir exists (for /int paths) */
        if(mkdir(SD_MOUNT_POINT "/.int", 0755) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "mkdir %s/.int failed: %s", SD_MOUNT_POINT, strerror(errno));
        }

        StorageEvent event = {.type = StorageEventTypeCardMount};
        furi_pubsub_publish(storage->pubsub, &event);
    } else {
        storage->sd_mounted = false;
        ESP_LOGW(TAG, "SD card mount failed — continuing without SD");

        StorageEvent event = {.type = StorageEventTypeCardMountError};
        furi_pubsub_publish(storage->pubsub, &event);
    }

    /* Register the storage record */
    furi_record_create(RECORD_STORAGE, storage);
    FURI_LOG_I(TAG, "Storage service started (sd_mounted=%d)", storage->sd_mounted);

    /* Service stays alive forever */
    while(true) {
        furi_delay_ms(1000);
    }

    return 0;
}

void storage_on_system_start(void) {
}
