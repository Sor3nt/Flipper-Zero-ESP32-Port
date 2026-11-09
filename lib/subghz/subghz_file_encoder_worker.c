#include "subghz_file_encoder_worker.h"

#include <toolbox/stream/stream.h>
#include <flipper_format/flipper_format.h>
#include <flipper_format/flipper_format_i.h>
#include <lib/subghz/devices/devices.h>
#include <lib/toolbox/strint.h>
#include <furi.h>
#include <stdatomic.h>

#define TAG "SubGhzFileEncoderWorker"

#define SUBGHZ_FILE_ENCODER_LOAD 512

struct SubGhzFileEncoderWorker {
    FuriThread* thread;
    FuriStreamBuffer* stream;
    FuriMutex* callback_mutex;

    Storage* storage;
    FlipperFormat* flipper_format;

    atomic_bool worker_running;
    atomic_bool worker_stopping;
    atomic_bool is_storage_slow;
    bool file_open;
    bool callback_end_sent;
    size_t progress_current;
    size_t progress_total;
    FuriString* str_data;
    FuriString* file_path;
    const SubGhzDevice* device;

    SubGhzFileEncoderWorkerCallbackEnd callback_end;
    void* context_end;
};

void subghz_file_encoder_worker_callback_end(
    SubGhzFileEncoderWorker* instance,
    SubGhzFileEncoderWorkerCallbackEnd callback_end,
    void* context_end) {
    furi_assert(instance);
    furi_assert(callback_end);
    furi_mutex_acquire(instance->callback_mutex, FuriWaitForever);
    instance->callback_end = callback_end;
    instance->context_end = context_end;
    furi_mutex_release(instance->callback_mutex);
}

void subghz_file_encoder_worker_add_level_duration(
    SubGhzFileEncoderWorker* instance,
    int32_t duration) {
    size_t ret = furi_stream_buffer_send(instance->stream, &duration, sizeof(int32_t), 100);
    if(sizeof(int32_t) != ret) FURI_LOG_E(TAG, "Invalid add duration in the stream");
}

bool subghz_file_encoder_worker_data_parse(SubGhzFileEncoderWorker* instance, const char* strStart) {
    // Line sample: "RAW_Data: -1, 2, -2..."

    // Look for the key in the line
    char* str = strstr(strStart, "RAW_Data: ");
    bool res = false;

    if(str) {
        // Skip key
        str = strchr(str, ' ');

        // Parse next element
        int32_t duration;
        while(strint_to_int32(str, &str, &duration, 10) == StrintParseNoError) {
            if((duration < -1000000) || (duration > 1000000)) {
                if(duration > 0) {
                    subghz_file_encoder_worker_add_level_duration(instance, (int32_t)100);
                } else {
                    subghz_file_encoder_worker_add_level_duration(instance, (int32_t)-100);
                }
                //FURI_LOG_I("PARSE", "Number overflow - %d", duration);
            } else {
                subghz_file_encoder_worker_add_level_duration(instance, duration);
            }
            if(*str == ',') str++; // could also be `\0`
        }

        res = true;
    }

    return res;
}

void subghz_file_encoder_worker_get_text_progress(
    SubGhzFileEncoderWorker* instance,
    FuriString* output) {
    furi_check(instance);
    furi_check(output);

    size_t current = 0;
    size_t total = 0;
    furi_mutex_acquire(instance->callback_mutex, FuriWaitForever);
    current = instance->progress_current;
    total = instance->progress_total;
    furi_mutex_release(instance->callback_mutex);

    size_t percent = total ? (100ULL * current) / total : 0;
    if(percent > 100) percent = 100;
    furi_string_printf(output, "%03u%%", (unsigned)percent);
}

LevelDuration subghz_file_encoder_worker_get_level_duration(void* context) {
    furi_assert(context);
    SubGhzFileEncoderWorker* instance = context;
    int32_t duration;
    int ret = furi_stream_buffer_receive(instance->stream, &duration, sizeof(int32_t), 0);
    if(ret == sizeof(int32_t)) {
        LevelDuration level_duration = {.level = LEVEL_DURATION_RESET};
        if(duration < 0) {
            level_duration = level_duration_make(false, -duration);
        } else if(duration > 0) {
            level_duration = level_duration_make(true, duration);
        } else if(duration == 0) { //-V547
            level_duration = level_duration_reset();
            FURI_LOG_I(TAG, "Stop transmission");
        atomic_store_explicit(&instance->worker_stopping, true, memory_order_relaxed);
        }
        return level_duration;
    } else {
        atomic_store_explicit(&instance->is_storage_slow, true, memory_order_relaxed);
        return level_duration_wait();
    }
}

/** Worker thread
 * 
 * @param context 
 * @return exit code 
 */
static int32_t subghz_file_encoder_worker_thread(void* context) {
    SubGhzFileEncoderWorker* instance = context;
    FURI_LOG_I(TAG, "Worker start");
    atomic_store_explicit(&instance->is_storage_slow, false, memory_order_relaxed);
    Stream* stream = flipper_format_get_raw_stream(instance->flipper_format);

    while(atomic_load_explicit(&instance->worker_running, memory_order_relaxed)) {
        size_t stream_free_byte = furi_stream_buffer_spaces_available(instance->stream);
        if((stream_free_byte / sizeof(int32_t)) >= SUBGHZ_FILE_ENCODER_LOAD) {
            if(stream_read_line(stream, instance->str_data)) {
                furi_string_trim(instance->str_data);
                if(!subghz_file_encoder_worker_data_parse(
                       instance, furi_string_get_cstr(instance->str_data))) {
                    subghz_file_encoder_worker_add_level_duration(instance, LEVEL_DURATION_RESET);
                    break;
                }
                furi_mutex_acquire(instance->callback_mutex, FuriWaitForever);
                instance->progress_current = stream_tell(stream);
                furi_mutex_release(instance->callback_mutex);
            } else {
                subghz_file_encoder_worker_add_level_duration(instance, LEVEL_DURATION_RESET);
                break;
            }
        } else {
            furi_delay_ms(1);
        }
    }
    //waiting for the end of the transfer
    if(atomic_load_explicit(&instance->is_storage_slow, memory_order_relaxed)) {
        FURI_LOG_E(TAG, "Storage is slow");
    }

    FURI_LOG_I(TAG, "End read file");
    while(instance->device && !subghz_devices_is_async_complete_tx(instance->device) &&
          atomic_load_explicit(&instance->worker_running, memory_order_relaxed)) {
        furi_delay_ms(5);
    }

    FURI_LOG_I(TAG, "End transmission");
    while(atomic_load_explicit(&instance->worker_running, memory_order_relaxed)) {
        SubGhzFileEncoderWorkerCallbackEnd callback_end = NULL;
        void* context_end = NULL;
        if(atomic_load_explicit(&instance->worker_stopping, memory_order_relaxed) &&
           !instance->callback_end_sent) {
            furi_mutex_acquire(instance->callback_mutex, FuriWaitForever);
            callback_end = instance->callback_end;
            context_end = instance->context_end;
            if(callback_end) instance->callback_end_sent = true;
            furi_mutex_release(instance->callback_mutex);
            if(callback_end) callback_end(context_end);
        }
        furi_delay_ms(50);
    }
    if(instance->file_open) {
        flipper_format_file_close(instance->flipper_format);
        instance->file_open = false;
    }

    FURI_LOG_I(TAG, "Worker stop");
    return 0;
}

SubGhzFileEncoderWorker* subghz_file_encoder_worker_alloc(void) {
    SubGhzFileEncoderWorker* instance = calloc(1, sizeof(SubGhzFileEncoderWorker));
    if(!instance) return NULL;

    atomic_init(&instance->worker_running, false);
    atomic_init(&instance->worker_stopping, true);
    atomic_init(&instance->is_storage_slow, false);

    instance->thread =
        furi_thread_alloc_ex("SubGhzFEWorker", 2048, subghz_file_encoder_worker_thread, instance);
    instance->stream = furi_stream_buffer_alloc(sizeof(int32_t) * 2048, sizeof(int32_t));
    instance->callback_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    instance->storage = furi_record_open(RECORD_STORAGE);
    if(instance->storage) {
        instance->flipper_format = flipper_format_file_alloc(instance->storage);
    }

    instance->str_data = furi_string_alloc();
    instance->file_path = furi_string_alloc();
    if(!instance->thread || !instance->stream || !instance->callback_mutex || !instance->storage ||
       !instance->flipper_format || !instance->str_data || !instance->file_path) {
        FURI_LOG_E(TAG, "Unable to allocate RAW file encoder worker resources");
        if(instance->file_path) furi_string_free(instance->file_path);
        if(instance->str_data) furi_string_free(instance->str_data);
        if(instance->flipper_format) flipper_format_free(instance->flipper_format);
        if(instance->storage) furi_record_close(RECORD_STORAGE);
        if(instance->callback_mutex) furi_mutex_free(instance->callback_mutex);
        if(instance->stream) furi_stream_buffer_free(instance->stream);
        if(instance->thread) furi_thread_free(instance->thread);
        free(instance);
        return NULL;
    }
    atomic_store_explicit(&instance->worker_stopping, true, memory_order_relaxed);

    return instance;
}

void subghz_file_encoder_worker_free(SubGhzFileEncoderWorker* instance) {
    furi_assert(instance);
    furi_check(!atomic_load_explicit(&instance->worker_running, memory_order_relaxed));

    if(instance->stream) furi_stream_buffer_free(instance->stream);
    if(instance->thread) furi_thread_free(instance->thread);

    furi_string_free(instance->str_data);
    furi_string_free(instance->file_path);
    if(instance->callback_mutex) furi_mutex_free(instance->callback_mutex);

    if(instance->flipper_format) flipper_format_free(instance->flipper_format);
    if(instance->storage) furi_record_close(RECORD_STORAGE);

    free(instance);
}

bool subghz_file_encoder_worker_start(
    SubGhzFileEncoderWorker* instance,
    const char* file_path,
    const char* radio_device_name) {
    furi_assert(instance);
    furi_assert(!atomic_load_explicit(&instance->worker_running, memory_order_relaxed));

    furi_stream_buffer_reset(instance->stream);
    furi_string_set(instance->file_path, file_path);
    instance->device = radio_device_name ? subghz_devices_get_by_name(radio_device_name) : NULL;
    atomic_store_explicit(&instance->is_storage_slow, false, memory_order_relaxed);
    atomic_store_explicit(&instance->worker_stopping, false, memory_order_relaxed);
    instance->callback_end_sent = false;

    if(!flipper_format_file_open_existing(
           instance->flipper_format, furi_string_get_cstr(instance->file_path))) {
        FURI_LOG_E(
            TAG,
            "Unable to open file for read: %s",
            furi_string_get_cstr(instance->file_path));
        return false;
    }
    instance->file_open = true;

    if(!flipper_format_read_string(instance->flipper_format, "Protocol", instance->str_data)) {
        FURI_LOG_E(TAG, "Missing Protocol");
        flipper_format_file_close(instance->flipper_format);
        instance->file_open = false;
        return false;
    }

    Stream* stream = flipper_format_get_raw_stream(instance->flipper_format);
    if(!stream_seek(stream, 1, StreamOffsetFromCurrent)) {
        FURI_LOG_E(TAG, "Unable to seek past Protocol line");
        flipper_format_file_close(instance->flipper_format);
        instance->file_open = false;
        return false;
    }

    furi_mutex_acquire(instance->callback_mutex, FuriWaitForever);
    instance->progress_total = stream_size(stream);
    instance->progress_current = stream_tell(stream);
    furi_mutex_release(instance->callback_mutex);

    FURI_LOG_I(TAG, "Start transmission");
    atomic_store_explicit(&instance->worker_running, true, memory_order_relaxed);
    furi_thread_start(instance->thread);

    return true;
}

void subghz_file_encoder_worker_stop(SubGhzFileEncoderWorker* instance) {
    furi_assert(instance);
    furi_assert(atomic_load_explicit(&instance->worker_running, memory_order_relaxed));

    atomic_store_explicit(&instance->worker_running, false, memory_order_relaxed);
    furi_thread_join(instance->thread);
}

bool subghz_file_encoder_worker_is_running(SubGhzFileEncoderWorker* instance) {
    furi_assert(instance);
    return atomic_load_explicit(&instance->worker_running, memory_order_relaxed);
}
