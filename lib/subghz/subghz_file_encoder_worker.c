#include "subghz_file_encoder_worker.h"

#include <toolbox/stream/stream.h>
#include <flipper_format/flipper_format.h>
#include <flipper_format/flipper_format_i.h>
#include <lib/subghz/devices/devices.h>
#include <lib/toolbox/strint.h>

#define TAG "SubGhzFileEncoderWorker"

#define SUBGHZ_FILE_ENCODER_LOAD 512

struct SubGhzFileEncoderWorker {
    FuriThread* thread;
    FuriStreamBuffer* stream;

    Storage* storage;
    FlipperFormat* flipper_format;

    volatile bool worker_running;
    volatile bool worker_stopping;
    volatile bool producer_finished;
    volatile bool producer_failed;
    volatile bool has_samples;
    volatile bool is_storage_slow;
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
    // Registered while the worker thread may already poll it: publish the
    // context before the callback so the thread never pairs it with NULL.
    __atomic_store_n(&instance->context_end, context_end, __ATOMIC_RELEASE);
    __atomic_store_n(&instance->callback_end, callback_end, __ATOMIC_RELEASE);
}

static bool subghz_file_encoder_worker_add_level_duration(
    SubGhzFileEncoderWorker* instance,
    int32_t duration) {
    size_t ret = furi_stream_buffer_send(instance->stream, &duration, sizeof(int32_t), 100);
    if(sizeof(int32_t) != ret) {
        FURI_LOG_E(TAG, "Unable to queue raw signal duration");
        return false;
    }
    return true;
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
                    if(!subghz_file_encoder_worker_add_level_duration(instance, (int32_t)100))
                        return false;
                } else {
                    if(!subghz_file_encoder_worker_add_level_duration(instance, (int32_t)-100))
                        return false;
                }
                instance->has_samples = true;
                //FURI_LOG_I("PARSE", "Number overflow - %d", duration);
            } else if(duration != 0) {
                // A 0 in the stream is the end-of-signal marker (LEVEL_DURATION_RESET);
                // queuing a stray 0 from the file would stop the transmission early.
                if(!subghz_file_encoder_worker_add_level_duration(instance, duration))
                    return false;
                instance->has_samples = true;
            }
            if(*str == ',') str++; // could also be `\0`
        }

        res = true;
    } else {
        // Ignore other FlipperFormat lines; only RAW_Data entries contain
        // signal durations, and metadata may occur before or after them.
        res = true;
    }

    return res;
}

void subghz_file_encoder_worker_get_text_progress(
    SubGhzFileEncoderWorker* instance,
    FuriString* output) {
    UNUSED(output);
    Stream* stream = flipper_format_get_raw_stream(instance->flipper_format);
    size_t total_size = stream_size(stream);
    size_t current_offset = stream_tell(stream);
    size_t buffer_avail = furi_stream_buffer_bytes_available(instance->stream);

    if(total_size == 0) {
        furi_string_set_str(output, "000%");
        return;
    }

    size_t consumed = current_offset > buffer_avail ? current_offset - buffer_avail : 0;
    if(consumed > total_size) consumed = total_size;
    furi_string_printf(output, "%03u%%", 100 * consumed / total_size);
}

LevelDuration subghz_file_encoder_worker_get_level_duration(void* context) {
    furi_assert(context);
    SubGhzFileEncoderWorker* instance = context;
    int32_t duration;
    // Sample the EOF flag BEFORE reading: the producer sets it only after its last
    // send, so "finished before an empty read" really means nothing is left.
    // Checking it after the read could see a flag set while the final samples
    // were queued in between, and cut off the end of the signal.
    const bool producer_finished = instance->producer_finished;
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
            instance->worker_stopping = true;
        }
        return level_duration;
    } else {
        if(producer_finished) {
            instance->worker_stopping = true;
            return level_duration_reset();
        }
        instance->is_storage_slow = true;
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
    bool res = false;
    instance->is_storage_slow = false;
    Stream* stream = flipper_format_get_raw_stream(instance->flipper_format);
    do {
        if(!flipper_format_file_open_existing(
               instance->flipper_format, furi_string_get_cstr(instance->file_path))) {
            FURI_LOG_E(
                TAG,
                "Unable to open file for read: %s",
                furi_string_get_cstr(instance->file_path));
            instance->producer_failed = true;
            break;
        }
        if(!flipper_format_read_string(instance->flipper_format, "Protocol", instance->str_data)) {
            FURI_LOG_E(TAG, "Missing Protocol");
            instance->producer_failed = true;
            break;
        }

        //skip the end of the previous line "\n"
        stream_seek(stream, 1, StreamOffsetFromCurrent);
        res = true;
        instance->worker_stopping = false;
        FURI_LOG_I(TAG, "Start transmission");
    } while(0);

    while(res && instance->worker_running) {
        size_t stream_free_byte = furi_stream_buffer_spaces_available(instance->stream);
        if((stream_free_byte / sizeof(int32_t)) >= SUBGHZ_FILE_ENCODER_LOAD) {
            if(stream_read_line(stream, instance->str_data)) {
                furi_string_trim(instance->str_data);
                if(!subghz_file_encoder_worker_data_parse(
                       instance, furi_string_get_cstr(instance->str_data))) {
                    (void)subghz_file_encoder_worker_add_level_duration(
                        instance, LEVEL_DURATION_RESET);
                    instance->producer_failed = true;
                    break;
                }
            } else {
                if(!subghz_file_encoder_worker_add_level_duration(
                       instance, LEVEL_DURATION_RESET)) {
                    instance->producer_failed = true;
                }
                break;
            }
        } else {
            furi_delay_ms(1);
        }
    }
    instance->producer_finished = true;
    FURI_LOG_I(TAG, "End read file");
    while(instance->device && !subghz_devices_is_async_complete_tx(instance->device) &&
          instance->worker_running) {
        furi_delay_ms(5);
    }
    if(instance->is_storage_slow) {
        FURI_LOG_E(TAG, "Raw signal data underrun during transmission");
    }

    FURI_LOG_I(TAG, "End transmission");
    while(instance->worker_running) {
        // worker_stopping means the RMT took the end marker, but up to one RMT
        // block of symbols is still going out. The caller turns the radio off
        // in response to callback_end, so only report the end once TX is
        // complete. (The wait above can be skipped: a small file is fully read
        // before TX starts, while "complete" still reads true from the last TX.)
        if(instance->worker_stopping &&
           (!instance->device || subghz_devices_is_async_complete_tx(instance->device))) {
            // The app registers callback_end right after TX starts; a short file
            // can finish before that. Keep the end pending until someone listens,
            // otherwise TX would never be reported as finished (status stuck on TX).
            SubGhzFileEncoderWorkerCallbackEnd callback =
                __atomic_load_n(&instance->callback_end, __ATOMIC_ACQUIRE);
            if(callback) {
                instance->worker_stopping = false;
                callback(__atomic_load_n(&instance->context_end, __ATOMIC_ACQUIRE));
            }
        }
        furi_delay_ms(50);
    }
    flipper_format_file_close(instance->flipper_format);

    FURI_LOG_I(TAG, "Worker stop");
    return 0;
}

SubGhzFileEncoderWorker* subghz_file_encoder_worker_alloc(void) {
    SubGhzFileEncoderWorker* instance = malloc(sizeof(SubGhzFileEncoderWorker));
    if(!instance) return NULL;

    instance->thread =
        furi_thread_alloc_ex("SubGhzFEWorker", 2048, subghz_file_encoder_worker_thread, instance);
    instance->stream = furi_stream_buffer_alloc(sizeof(int32_t) * 2048, sizeof(int32_t));
    if(!instance->thread || !instance->stream) {
        if(instance->stream) furi_stream_buffer_free(instance->stream);
        if(instance->thread) furi_thread_free(instance->thread);
        free(instance);
        return NULL;
    }

    instance->storage = furi_record_open(RECORD_STORAGE);
    instance->flipper_format = flipper_format_file_alloc(instance->storage);

    instance->str_data = furi_string_alloc();
    instance->file_path = furi_string_alloc();
    if(!instance->flipper_format || !instance->str_data || !instance->file_path) {
        if(instance->file_path) furi_string_free(instance->file_path);
        if(instance->str_data) furi_string_free(instance->str_data);
        if(instance->flipper_format) flipper_format_free(instance->flipper_format);
        furi_record_close(RECORD_STORAGE);
        furi_stream_buffer_free(instance->stream);
        furi_thread_free(instance->thread);
        free(instance);
        return NULL;
    }
    instance->worker_running = false;
    instance->worker_stopping = true;
    instance->producer_finished = false;
    instance->producer_failed = false;
    instance->has_samples = false;
    instance->is_storage_slow = false;
    instance->device = NULL;
    instance->callback_end = NULL;
    instance->context_end = NULL;

    return instance;
}

void subghz_file_encoder_worker_free(SubGhzFileEncoderWorker* instance) {
    furi_assert(instance);

    if(instance->worker_running) {
        instance->worker_running = false;
        furi_thread_join(instance->thread);
    }
    furi_stream_buffer_free(instance->stream);
    furi_thread_free(instance->thread);

    furi_string_free(instance->str_data);
    furi_string_free(instance->file_path);

    flipper_format_free(instance->flipper_format);
    furi_record_close(RECORD_STORAGE);

    free(instance);
}

bool subghz_file_encoder_worker_start(
    SubGhzFileEncoderWorker* instance,
    const char* file_path,
    const char* radio_device_name) {
    furi_assert(instance);
    furi_assert(!instance->worker_running);

    furi_stream_buffer_reset(instance->stream);
    furi_string_set(instance->file_path, file_path);
    if(radio_device_name) {
        instance->device = subghz_devices_get_by_name(radio_device_name);
    }
    instance->producer_finished = false;
    instance->producer_failed = false;
    instance->has_samples = false;
    instance->worker_running = true;
    furi_thread_start(instance->thread);

    return true;
}

SubGhzFileEncoderWorkerPreloadResult subghz_file_encoder_worker_wait_for_data(
    SubGhzFileEncoderWorker* instance,
    size_t min_samples,
    uint32_t timeout_ms) {
    furi_assert(instance);
    const uint32_t start_tick = furi_get_tick();
    const uint32_t timeout_ticks = furi_ms_to_ticks(timeout_ms);

    do {
        const size_t samples =
            furi_stream_buffer_bytes_available(instance->stream) / sizeof(int32_t);
        if(instance->producer_failed) return SubGhzFileEncoderWorkerPreloadFailed;
        if(samples >= min_samples) return SubGhzFileEncoderWorkerPreloadReady;
        if(instance->producer_finished) {
            return instance->has_samples && samples > 0 ?
                       SubGhzFileEncoderWorkerPreloadReady :
                       SubGhzFileEncoderWorkerPreloadFailed;
        }
        if((uint32_t)(furi_get_tick() - start_tick) >= timeout_ticks) {
            return instance->worker_running ? SubGhzFileEncoderWorkerPreloadTimeout :
                                              SubGhzFileEncoderWorkerPreloadFailed;
        }
        furi_delay_ms(1);
    } while(instance->worker_running);

    return SubGhzFileEncoderWorkerPreloadFailed;
}

void subghz_file_encoder_worker_stop(SubGhzFileEncoderWorker* instance) {
    furi_assert(instance);
    furi_assert(instance->worker_running);

    instance->worker_running = false;
    furi_thread_join(instance->thread);
}

bool subghz_file_encoder_worker_is_running(SubGhzFileEncoderWorker* instance) {
    furi_assert(instance);
    return instance->worker_running;
}
