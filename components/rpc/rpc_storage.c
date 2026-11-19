#include <core/common_defines.h>
#include <core/memmgr.h>
#include <core/record.h>
#include "rpc.h"
#include "rpc_i.h"
#include <storage/filesystem_api_defines.h>
#include <storage/storage.h>
#include <toolbox/md5_calc.h>
#include <toolbox/path.h>

#include <pb_decode.h>
#include <storage.pb.h>
#include <flipper.pb.h>
#include <esp_log.h>
#include <furi_hal_qfdiag.h>

#define TAG "RpcStorage"

/* Per-chunk write trace: 2-3 lines per 1 KB upload chunk at INFO, so DEBUG
 * (compiled out) unless QFDIAG is enabled. */
#if QFDIAG_ENABLED
#define RPC_STORAGE_TRACE(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define RPC_STORAGE_TRACE(...) ESP_LOGD(TAG, __VA_ARGS__)
#endif

#define MAX_NAME_LENGTH 255

static const size_t MAX_DATA_SIZE = 512;

typedef enum {
    RpcStorageStateIdle = 0,
    RpcStorageStateWriting,
} RpcStorageState;

typedef struct {
    RpcSession* session;
    Storage* api;
    File* file;
    RpcStorageState state;
    uint32_t current_command_id;
} RpcStorageSystem;

static void rpc_system_storage_reset_state(
    RpcStorageSystem* rpc_storage,
    RpcSession* session,
    bool send_error) {
    furi_assert(rpc_storage);
    furi_assert(session);

    if(rpc_storage->state != RpcStorageStateIdle) {
        if(send_error) {
            rpc_send_and_release_empty(
                session,
                rpc_storage->current_command_id,
                PB_CommandStatus_ERROR_CONTINUOUS_COMMAND_INTERRUPTED);
        }

        if(rpc_storage->state == RpcStorageStateWriting) {
            storage_file_close(rpc_storage->file);
            storage_file_free(rpc_storage->file);
        }

        rpc_storage->state = RpcStorageStateIdle;
    }
}

PB_CommandStatus rpc_system_storage_get_error(FS_Error fs_error) {
    PB_CommandStatus pb_error;
    switch(fs_error) {
    case FSE_OK:
        pb_error = PB_CommandStatus_OK;
        break;
    case FSE_INVALID_NAME:
        pb_error = PB_CommandStatus_ERROR_STORAGE_INVALID_NAME;
        break;
    case FSE_INVALID_PARAMETER:
        pb_error = PB_CommandStatus_ERROR_STORAGE_INVALID_PARAMETER;
        break;
    case FSE_INTERNAL:
        pb_error = PB_CommandStatus_ERROR_STORAGE_INTERNAL;
        break;
    case FSE_ALREADY_OPEN:
        pb_error = PB_CommandStatus_ERROR_STORAGE_ALREADY_OPEN;
        break;
    case FSE_DENIED:
        pb_error = PB_CommandStatus_ERROR_STORAGE_DENIED;
        break;
    case FSE_EXIST:
        pb_error = PB_CommandStatus_ERROR_STORAGE_EXIST;
        break;
    case FSE_NOT_EXIST:
        pb_error = PB_CommandStatus_ERROR_STORAGE_NOT_EXIST;
        break;
    case FSE_NOT_READY:
        pb_error = PB_CommandStatus_ERROR_STORAGE_NOT_READY;
        break;
    case FSE_NOT_IMPLEMENTED:
        pb_error = PB_CommandStatus_ERROR_STORAGE_NOT_IMPLEMENTED;
        break;
    default:
        pb_error = PB_CommandStatus_ERROR;
        break;
    }

    return pb_error;
}

static PB_CommandStatus rpc_system_storage_get_file_error(File* file) {
    return rpc_system_storage_get_error(storage_file_get_error(file));
}

static void rpc_system_storage_info_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);
    furi_assert(request->which_content == PB_Main_storage_info_request_tag);

    FURI_LOG_D(TAG, "Info");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    rpc_system_storage_reset_state(rpc_storage, session, true);

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;

    FS_Error error = storage_common_fs_info(
        rpc_storage->api,
        request->content.storage_info_request.path,
        &response->content.storage_info_response.total_space,
        &response->content.storage_info_response.free_space);

    response->command_status = rpc_system_storage_get_error(error);
    if(error == FSE_OK) {
        response->which_content = PB_Main_storage_info_response_tag;
    } else {
        response->which_content = PB_Main_empty_tag;
    }

    rpc_send_and_release(session, response);
    free(response);
}

static void rpc_system_storage_timestamp_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);
    furi_assert(request->which_content == PB_Main_storage_timestamp_request_tag);

    FURI_LOG_D(TAG, "Timestamp");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    rpc_system_storage_reset_state(rpc_storage, session, true);

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;

    const char* path = request->content.storage_timestamp_request.path;
    uint32_t timestamp = 0;
    FS_Error error = storage_common_timestamp(rpc_storage->api, path, &timestamp);

    response->command_status = rpc_system_storage_get_error(error);
    response->which_content = PB_Main_empty_tag;

    if(error == FSE_OK) {
        response->which_content = PB_Main_storage_timestamp_response_tag;
        response->content.storage_timestamp_response.timestamp = timestamp;
    }

    rpc_send_and_release(session, response);
    free(response);
}

static void rpc_system_storage_stat_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);
    furi_assert(request->which_content == PB_Main_storage_stat_request_tag);

    FURI_LOG_D(TAG, "Stat");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    rpc_system_storage_reset_state(rpc_storage, session, true);

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;

    const char* path = request->content.storage_stat_request.path;
    FileInfo fileinfo;
    FS_Error error = storage_common_stat(rpc_storage->api, path, &fileinfo);

    response->command_status = rpc_system_storage_get_error(error);
    response->which_content = PB_Main_empty_tag;

    if(error == FSE_OK) {
        response->which_content = PB_Main_storage_stat_response_tag;
        response->content.storage_stat_response.has_file = true;
        response->content.storage_stat_response.file.type = file_info_is_dir(&fileinfo) ?
                                                                PB_Storage_File_FileType_DIR :
                                                                PB_Storage_File_FileType_FILE;
        response->content.storage_stat_response.file.size = fileinfo.size;
    }

    rpc_send_and_release(session, response);
    free(response);
}

static void rpc_system_storage_list_root(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    const char* hard_coded_dirs[] = {"any", "int", "ext"};

    PB_Main response = {
        .has_next = false,
        .command_id = request->command_id,
        .command_status = PB_CommandStatus_OK,
        .which_content = PB_Main_storage_list_response_tag,
    };
    furi_assert(COUNT_OF(hard_coded_dirs) < COUNT_OF(response.content.storage_list_response.file));

    for(uint32_t i = 0; i < COUNT_OF(hard_coded_dirs); ++i) {
        ++response.content.storage_list_response.file_count;
        response.content.storage_list_response.file[i].data = NULL;
        response.content.storage_list_response.file[i].size = 0;
        response.content.storage_list_response.file[i].type = PB_Storage_File_FileType_DIR;
        response.content.storage_list_response.file[i].name = strdup(hard_coded_dirs[i]);
    }

    rpc_send_and_release(session, &response);
}

static bool rpc_system_storage_list_filter(
    const PB_Storage_ListRequest* request,
    const FileInfo* fileinfo,
    const char* name) {
    bool result = false;

    do {
        if(!path_contains_only_ascii(name)) break;
        if(request->filter_max_size) {
            if(fileinfo->size > request->filter_max_size) break;
        }
        result = true;
    } while(false);

    return result;
}

static void rpc_system_storage_list_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);
    furi_assert(request->which_content == PB_Main_storage_list_request_tag);

    FURI_LOG_D(TAG, "List");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    const PB_Storage_ListRequest* list_request = &request->content.storage_list_request;

    rpc_system_storage_reset_state(rpc_storage, session, true);

    if(!strcmp(list_request->path, "/")) {
        rpc_system_storage_list_root(request, context);
        return;
    }

    File* dir = storage_file_alloc(rpc_storage->api);

    PB_Main response = {
        .command_id = request->command_id,
        .has_next = false,
        .which_content = PB_Main_storage_list_response_tag,
        .command_status = PB_CommandStatus_OK,
    };
    PB_Storage_ListResponse* list = &response.content.storage_list_response;

    bool include_md5 = list_request->include_md5;
    FuriString* md5 = furi_string_alloc();
    FuriString* md5_path = furi_string_alloc();
    File* file = storage_file_alloc(rpc_storage->api);

    bool finish = false;
    int i = 0;

    if(!storage_dir_open(dir, list_request->path)) {
        response.command_status = rpc_system_storage_get_file_error(dir);
        response.which_content = PB_Main_empty_tag;
        finish = true;
    }

    while(!finish) { //-V1044
        FileInfo fileinfo;
        char* name = malloc(MAX_NAME_LENGTH + 1);
        if(storage_dir_read(dir, &fileinfo, name, MAX_NAME_LENGTH)) {
            if(rpc_system_storage_list_filter(list_request, &fileinfo, name)) {
                if(i == COUNT_OF(list->file)) {
                    list->file_count = i;
                    response.has_next = true;
                    rpc_send_and_release(session, &response);
                    i = 0;
                }
                list->file[i].type = file_info_is_dir(&fileinfo) ? PB_Storage_File_FileType_DIR :
                                                                   PB_Storage_File_FileType_FILE;
                list->file[i].size = fileinfo.size;
                list->file[i].data = NULL;
                list->file[i].name = name;

                if(include_md5 && !file_info_is_dir(&fileinfo)) {
                    furi_string_printf(md5_path, "%s/%s", list_request->path, name); //-V576

                    if(md5_string_calc_file(file, furi_string_get_cstr(md5_path), md5, NULL)) {
                        char* md5sum = list->file[i].md5sum;
                        size_t md5sum_size = sizeof(list->file[i].md5sum);
                        snprintf(md5sum, md5sum_size, "%s", furi_string_get_cstr(md5));
                    }
                }

                ++i;
            } else {
                free(name);
            }
        } else {
            list->file_count = i;
            finish = true;
            free(name);
        }
    }

    response.has_next = false;
    rpc_send_and_release(session, &response);

    furi_string_free(md5);
    furi_string_free(md5_path);
    storage_dir_close(dir);
    storage_file_free(dir);
    storage_file_free(file);
}

static void rpc_system_storage_read_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);
    furi_assert(request->which_content == PB_Main_storage_read_request_tag);

    FURI_LOG_D(TAG, "Read");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    rpc_system_storage_reset_state(rpc_storage, session, true);

    /* use same message memory to send response */
    PB_Main* response = malloc(sizeof(PB_Main));
    const char* path = request->content.storage_read_request.path;
    File* file = storage_file_alloc(rpc_storage->api);
    bool fs_operation_success = storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING);

    if(fs_operation_success) {
        size_t size_left = storage_file_size(file);
        do {
            response->command_id = request->command_id;
            response->which_content = PB_Main_storage_read_response_tag;
            response->command_status = PB_CommandStatus_OK;

            size_t read_size = MIN(size_left, MAX_DATA_SIZE);
            if(read_size) {
                response->content.storage_read_response.has_file = true;
                response->content.storage_read_response.file.data =
                    malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(read_size));
                uint8_t* buffer = &response->content.storage_read_response.file.data->bytes[0];
                uint16_t* read_size_msg = &response->content.storage_read_response.file.data->size;

                *read_size_msg = storage_file_read(file, buffer, read_size);
                size_left -= *read_size_msg;
                fs_operation_success = (*read_size_msg == read_size);

                response->has_next = fs_operation_success && (size_left > 0);
            } else {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
                // nanopb flexible-array-Idiom: ALLOCSIZE(0) belegt bewusst nur das size-Feld;
                // GCC sieht das als zu kleine Allokation fuer den Gesamttyp -> false positive.
#pragma GCC diagnostic ignored "-Walloc-size"
                response->content.storage_read_response.file.data =
                    malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(0));
                response->content.storage_read_response.file.data->size = 0;
#pragma GCC diagnostic pop
                response->content.storage_read_response.has_file = true;
                response->has_next = false;
                fs_operation_success = true;
            }

            if(fs_operation_success) {
                rpc_send_and_release(session, response);
            }
        } while((size_left != 0) && fs_operation_success);
    }

    if(!fs_operation_success) {
        rpc_send_and_release_empty(
            session, request->command_id, rpc_system_storage_get_file_error(file));
    }

    free(response);
    storage_file_close(file);
    storage_file_free(file);
}

static void rpc_system_storage_write_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);
    furi_assert(request->which_content == PB_Main_storage_write_request_tag);

    FURI_LOG_D(TAG, "Write");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    bool fs_operation_success = true;

    if(!path_contains_only_ascii(request->content.storage_write_request.path)) {
        rpc_storage->current_command_id = request->command_id;
        rpc_send_and_release_empty(
            session, rpc_storage->current_command_id, PB_CommandStatus_ERROR_STORAGE_INVALID_NAME);
        rpc_system_storage_reset_state(rpc_storage, session, false);
        return;
    }

    if((request->command_id != rpc_storage->current_command_id) &&
       (rpc_storage->state == RpcStorageStateWriting)) {
        rpc_system_storage_reset_state(rpc_storage, session, true);
    }

    if(rpc_storage->state != RpcStorageStateWriting) {
        rpc_storage->file = storage_file_alloc(rpc_storage->api);
        rpc_storage->current_command_id = request->command_id;
        rpc_storage->state = RpcStorageStateWriting;
        const char* path = request->content.storage_write_request.path;
        const uint32_t qd_o0 = QFDIAG_ENABLED ? furi_get_tick() : 0;
        fs_operation_success =
            storage_file_open(rpc_storage->file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
        if(QFDIAG_ENABLED) { /* QFDIAG: file open latency */
            ESP_LOGI(
                TAG,
                "QD write: open %s ok=%d in %lu ms",
                path,
                fs_operation_success,
                (unsigned long)(furi_get_tick() - qd_o0));
        }
    }

    File* file = rpc_storage->file;
    bool send_response = false;

    if(fs_operation_success) {
        if(request->content.storage_write_request.has_file &&
           request->content.storage_write_request.file.data &&
           request->content.storage_write_request.file.data->size) {
            uint8_t* buffer = request->content.storage_write_request.file.data->bytes;
            size_t buffer_size = request->content.storage_write_request.file.data->size;
            const uint32_t qd_w0 = QFDIAG_ENABLED ? furi_get_tick() : 0;
            size_t written_size = storage_file_write(file, buffer, buffer_size);
            const uint32_t qd_wd = QFDIAG_ENABLED ? furi_get_tick() - qd_w0 : 0;
            fs_operation_success = (written_size == buffer_size);
            if(QFDIAG_ENABLED) {
                /* QFDIAG: one summary line per 100 chunks — throughput and
                 * per-chunk storage latency, without flooding the log ring. */
                static uint32_t n, sum_ms, max_ms, bytes, win_start;
                if(!n) win_start = qd_w0;
                n++;
                sum_ms += qd_wd;
                if(qd_wd > max_ms) max_ms = qd_wd;
                bytes += (uint32_t)written_size;
                if(n >= 100) {
                    const uint32_t span = furi_get_tick() - win_start;
                    ESP_LOGI(
                        TAG,
                        "QD write: 100 chunks %lu B in %lu ms (%lu B/s), store avg %lu max %lu ms",
                        (unsigned long)bytes,
                        (unsigned long)span,
                        (unsigned long)(span ? (uint64_t)bytes * 1000U / span : 0),
                        (unsigned long)(sum_ms / n),
                        (unsigned long)max_ms);
                    n = sum_ms = max_ms = bytes = 0;
                }
            }
            if(QFDIAG_ENABLED && (qd_wd > 300 || !fs_operation_success)) {
                ESP_LOGW(
                    TAG,
                    "QD write: storage_file_write %u/%u bytes took %lu ms (fs_err=%d)",
                    (unsigned)written_size,
                    (unsigned)buffer_size,
                    (unsigned long)qd_wd,
                    (int)storage_file_get_error(file));
            }
        }

        send_response = !request->has_next;
    }

    PB_CommandStatus command_status = PB_CommandStatus_OK;
    if(!fs_operation_success) {
        send_response = true;
        command_status = rpc_system_storage_get_file_error(file);
        if(command_status == PB_CommandStatus_OK) {
            // Report errors not handled by underlying APIs
            command_status = PB_CommandStatus_ERROR_STORAGE_INTERNAL;
        }
    }

    RPC_STORAGE_TRACE("write: has_next=%d send_response=%d fs_ok=%d status=%d",
        request->has_next, send_response, fs_operation_success, command_status);

    if(send_response) {
        RPC_STORAGE_TRACE("write: sending final response cmd_id=%lu status=%d",
            (unsigned long)rpc_storage->current_command_id, command_status);
        rpc_send_and_release_empty(session, rpc_storage->current_command_id, command_status);
        rpc_system_storage_reset_state(rpc_storage, session, false);
        RPC_STORAGE_TRACE("write: response sent OK");
    }
}

static bool rpc_system_storage_is_dir_is_empty(Storage* storage, const char* path) {
    furi_assert(storage);
    furi_assert(path);

    FileInfo fileinfo;
    bool is_dir_is_empty = true;
    FS_Error error = storage_common_stat(storage, path, &fileinfo);
    if((error == FSE_OK) && file_info_is_dir(&fileinfo)) {
        File* dir = storage_file_alloc(storage);
        if(storage_dir_open(dir, path)) {
            char* name = malloc(MAX_NAME_LENGTH);
            while(storage_dir_read(dir, &fileinfo, name, MAX_NAME_LENGTH)) {
                if(path_contains_only_ascii(name)) {
                    is_dir_is_empty = false;
                    break;
                }
            }
            free(name);
        }
        storage_dir_close(dir);
        storage_file_free(dir);
    }

    return is_dir_is_empty;
}

/* FIX [QF-4]: deleting a big folder (e.g. asset_packs) in qFlipper always
 *   ended in "Operation timeout" although the device finished the delete.
 * ROOT CAUSE: a recursive delete of thousands of files takes a minute or more
 *   (each FatFs unlink = ~4 synchronous single-sector SD writes: dir entry,
 *   FAT1, FAT2, FSInfo, often 100+ ms each), but qFlipper gives every operation
 *   a 30 s single-shot timeout (AbstractOperation: setInterval(30000)). It
 *   re-arms that timer only on a reply with the SAME command_id and
 *   has_next=true, and finishes only on has_next=false.
 * FIX: while the delete runs, send an Empty/OK has_next=true keep-alive every
 *   RPC_STORAGE_DELETE_KEEPALIVE_MS, then the real final reply (has_next=false).
 *   Deletes shorter than one interval send no keep-alive at all.
 * WARNING: keep the interval well below 30 s; the callback only runs between
 *   directory entries, so one slow unlink delays the next keep-alive. The
 *   keep-alive must be an Empty message (qFlipper's default processResponse
 *   rejects any other type) and must carry the request's command_id.
 * FIX [QF-3]: the same callback aborts the delete once the session is closing,
 *   so a host that left does not keep the global busy_mutex for minutes. */
#define RPC_STORAGE_DELETE_KEEPALIVE_MS (5000U)

typedef struct {
    RpcSession* session;
    uint32_t command_id;
    uint32_t last_reply_tick;
    uint32_t keepalives;
} RpcStorageDeleteProgress;

static bool rpc_system_storage_delete_should_abort(void* context) {
    RpcStorageDeleteProgress* progress = context;
    if(rpc_session_is_closing(progress->session)) return true;

    uint32_t now = furi_get_tick();
    if(now - progress->last_reply_tick >= furi_ms_to_ticks(RPC_STORAGE_DELETE_KEEPALIVE_MS)) {
        PB_Main keepalive = {
            .command_id = progress->command_id,
            .command_status = PB_CommandStatus_OK,
            .has_next = true,
            .which_content = PB_Main_empty_tag,
        };
        rpc_send_and_release(progress->session, &keepalive);
        progress->last_reply_tick = now;
        progress->keepalives++;
    }
    return false;
}

static void rpc_system_storage_delete_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_storage_delete_request_tag);
    furi_assert(context);

    FURI_LOG_D(TAG, "Delete");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    PB_CommandStatus status = PB_CommandStatus_ERROR;
    rpc_system_storage_reset_state(rpc_storage, session, true);

    char* path = request->content.storage_delete_request.path;
    if(!path) {
        status = PB_CommandStatus_ERROR_INVALID_PARAMETERS;
    } else {
        FS_Error error_remove = storage_common_remove(rpc_storage->api, path);
        // FSE_DENIED is for empty directory, but not only for this
        // that's why we have to check it
        if((error_remove == FSE_DENIED) &&
           !rpc_system_storage_is_dir_is_empty(rpc_storage->api, path)) {
            if(request->content.storage_delete_request.recursive) {
                RpcStorageDeleteProgress progress = {
                    .session = session,
                    .command_id = request->command_id,
                    .last_reply_tick = furi_get_tick(),
                    .keepalives = 0,
                };
                /* FIX [QF-3]/[QF-4]: must stay the abortable variant with the
                 * keep-alive callback; plain storage_simply_remove_recursive()
                 * brings both bugs back. */
                bool deleted = storage_simply_remove_recursive_abortable(
                    rpc_storage->api, path, rpc_system_storage_delete_should_abort, &progress);
                if(!deleted && rpc_session_is_closing(session)) {
                    FURI_LOG_W(TAG, "Delete of %s stopped: session closed", path);
                } else if(progress.keepalives) {
                    FURI_LOG_I(
                        TAG,
                        "Delete of %s: %lu keep-alive replies sent",
                        path,
                        (unsigned long)progress.keepalives);
                }
                status = deleted ? PB_CommandStatus_OK : PB_CommandStatus_ERROR;
            } else {
                status = PB_CommandStatus_ERROR_STORAGE_DIR_NOT_EMPTY;
            }
        } else if(error_remove == FSE_NOT_EXIST) {
            status = PB_CommandStatus_OK;
        } else {
            status = rpc_system_storage_get_error(error_remove);
        }
    }

    rpc_send_and_release_empty(session, request->command_id, status);
}

static void rpc_system_storage_mkdir_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_storage_mkdir_request_tag);
    furi_assert(context);

    FURI_LOG_D(TAG, "Mkdir");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    PB_CommandStatus status;
    rpc_system_storage_reset_state(rpc_storage, session, true);

    char* path = request->content.storage_mkdir_request.path;
    if(path) {
        if(path_contains_only_ascii(path)) {
            FS_Error error = storage_common_mkdir(rpc_storage->api, path);
            status = rpc_system_storage_get_error(error);
        } else {
            status = PB_CommandStatus_ERROR_STORAGE_INVALID_NAME;
        }
    } else {
        status = PB_CommandStatus_ERROR_INVALID_PARAMETERS;
    }
    rpc_send_and_release_empty(session, request->command_id, status);
}

static void rpc_system_storage_md5sum_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_storage_md5sum_request_tag);
    furi_assert(context);

    FURI_LOG_D(TAG, "Md5sum");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    rpc_system_storage_reset_state(rpc_storage, session, true);

    const char* filename = request->content.storage_md5sum_request.path;
    if(!filename) {
        rpc_send_and_release_empty(
            session, request->command_id, PB_CommandStatus_ERROR_INVALID_PARAMETERS);
        return;
    }

    File* file = storage_file_alloc(rpc_storage->api);
    FuriString* md5 = furi_string_alloc();
    FS_Error file_error;

    if(md5_string_calc_file(file, filename, md5, &file_error)) {
        PB_Main response = {
            .command_id = request->command_id,
            .command_status = PB_CommandStatus_OK,
            .which_content = PB_Main_storage_md5sum_response_tag,
            .has_next = false,
        };

        char* md5sum = response.content.storage_md5sum_response.md5sum;
        size_t md5sum_size = sizeof(response.content.storage_md5sum_response.md5sum);
        snprintf(md5sum, md5sum_size, "%s", furi_string_get_cstr(md5));

        rpc_send_and_release(session, &response);
    } else {
        rpc_send_and_release_empty(
            session, request->command_id, rpc_system_storage_get_error(file_error));
    }

    furi_string_free(md5);
    storage_file_free(file);
}

static void rpc_system_storage_rename_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_storage_rename_request_tag);
    furi_assert(context);

    FURI_LOG_D(TAG, "Rename");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    PB_CommandStatus status;
    rpc_system_storage_reset_state(rpc_storage, session, true);

    if(path_contains_only_ascii(request->content.storage_rename_request.new_path)) {
        FS_Error error = storage_common_rename(
            rpc_storage->api,
            request->content.storage_rename_request.old_path,
            request->content.storage_rename_request.new_path);
        status = rpc_system_storage_get_error(error);
    } else {
        status = PB_CommandStatus_ERROR_STORAGE_INVALID_NAME;
    }

    rpc_send_and_release_empty(session, request->command_id, status);
}

#if 0 /* Backup/tar not ported to ESP32 yet */
static void rpc_system_storage_backup_create_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_storage_backup_create_request_tag);
    furi_assert(context);

    FURI_LOG_D(TAG, "BackupCreate");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    rpc_system_storage_reset_state(rpc_storage, session, true);

    bool backup_ok = int_backup_create(
        rpc_storage->api, request->content.storage_backup_create_request.archive_path);

    rpc_send_and_release_empty(
        session, request->command_id, backup_ok ? PB_CommandStatus_OK : PB_CommandStatus_ERROR);
}

static void rpc_system_storage_backup_restore_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_storage_backup_restore_request_tag);
    furi_assert(context);

    FURI_LOG_D(TAG, "BackupRestore");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    rpc_system_storage_reset_state(rpc_storage, session, true);

    bool backup_ok = int_backup_unpack(
        rpc_storage->api, request->content.storage_backup_restore_request.archive_path);

    rpc_send_and_release_empty(
        session, request->command_id, backup_ok ? PB_CommandStatus_OK : PB_CommandStatus_ERROR);
}

static void rpc_system_storage_tar_extract_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_storage_tar_extract_request_tag);
    furi_assert(context);

    FURI_LOG_D(TAG, "TarExtract");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    PB_CommandStatus status;
    rpc_system_storage_reset_state(rpc_storage, session, true);

    TarArchive* archive = tar_archive_alloc(rpc_storage->api);

    do {
        const char *tar_path = request->content.storage_tar_extract_request.tar_path,
                   *out_path = request->content.storage_tar_extract_request.out_path;
        if(!path_contains_only_ascii(out_path)) {
            status = PB_CommandStatus_ERROR_STORAGE_INVALID_NAME;
            break;
        }

        TarOpenMode tar_mode = tar_archive_get_mode_for_path(tar_path);

        if(!tar_archive_open(archive, tar_path, tar_mode)) {
            status = PB_CommandStatus_ERROR_STORAGE_INVALID_PARAMETER;
            break;
        }

        if(!tar_archive_unpack_to(archive, out_path, NULL)) {
            status = PB_CommandStatus_ERROR_STORAGE_INTERNAL;
            break;
        }

        status = PB_CommandStatus_OK;
    } while(0);

    tar_archive_free(archive);
    rpc_send_and_release_empty(session, request->command_id, status);
}
#endif /* backup/tar */

void* rpc_system_storage_alloc(RpcSession* session) {
    furi_assert(session);

    RpcStorageSystem* rpc_storage = malloc(sizeof(RpcStorageSystem));
    rpc_storage->api = furi_record_open(RECORD_STORAGE);
    rpc_storage->session = session;
    rpc_storage->state = RpcStorageStateIdle;

    RpcHandler rpc_handler = {
        .message_handler = NULL,
        .decode_submessage = NULL,
        .context = rpc_storage,
    };

    rpc_handler.message_handler = rpc_system_storage_info_process;
    rpc_add_handler(session, PB_Main_storage_info_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_timestamp_process;
    rpc_add_handler(session, PB_Main_storage_timestamp_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_stat_process;
    rpc_add_handler(session, PB_Main_storage_stat_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_list_process;
    rpc_add_handler(session, PB_Main_storage_list_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_read_process;
    rpc_add_handler(session, PB_Main_storage_read_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_write_process;
    rpc_add_handler(session, PB_Main_storage_write_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_delete_process;
    rpc_add_handler(session, PB_Main_storage_delete_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_mkdir_process;
    rpc_add_handler(session, PB_Main_storage_mkdir_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_md5sum_process;
    rpc_add_handler(session, PB_Main_storage_md5sum_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_rename_process;
    rpc_add_handler(session, PB_Main_storage_rename_request_tag, &rpc_handler);

    /* Backup/tar not ported to ESP32 yet */

    return rpc_storage;
}

void rpc_system_storage_free(void* context) {
    furi_assert(context);
    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    rpc_system_storage_reset_state(rpc_storage, session, false);

    furi_record_close(RECORD_STORAGE);
    rpc_storage->api = NULL;
    free(rpc_storage);
}
