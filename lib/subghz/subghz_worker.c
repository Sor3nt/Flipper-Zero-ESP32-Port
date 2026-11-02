#include "subghz_worker.h"

#include <furi.h>

#define TAG "SubGhzWorker"

/* ISR -> worker queue depth, in LevelDuration samples. */
#define SUBGHZ_WORKER_STREAM_SAMPLES 4096

struct SubGhzWorker {
    FuriThread* thread;
    FuriStreamBuffer* stream;

    volatile bool running;
    volatile bool overrun;

    LevelDuration filter_level_duration;
    bool filter_has_data;
    uint16_t filter_duration;

    /* Per-session RX diagnostics (reset in start, logged in stop): how often the
     * ISR found the queue full (dropped signal) and how full it got. Used to
     * judge the worker priority with data instead of guessing. */
    uint32_t overrun_events;
    uint32_t samples;
    size_t stream_peak_bytes;

    SubGhzWorkerOverrunCallback overrun_callback;
    SubGhzWorkerPairCallback pair_callback;
    void* context;
};

/** Rx callback timer
 * 
 * @param level received signal level
 * @param duration received signal duration
 * @param context 
 */
void subghz_worker_rx_callback(bool level, uint32_t duration, void* context) {
    SubGhzWorker* instance = context;

    LevelDuration level_duration = level_duration_make(level, duration);
    if(instance->overrun) {
        instance->overrun = false;
        level_duration = level_duration_reset();
    }
    size_t ret = furi_stream_buffer_send_from_isr_yield(
        instance->stream, &level_duration, sizeof(LevelDuration));
    if(sizeof(LevelDuration) != ret) instance->overrun = true;
}

/** Worker callback thread
 * 
 * @param context 
 * @return exit code 
 */
static int32_t subghz_worker_thread_callback(void* context) {
    SubGhzWorker* instance = context;

    LevelDuration level_duration;

    while(instance->running) {
        int ret = furi_stream_buffer_receive(
            instance->stream, &level_duration, sizeof(LevelDuration), 10);
        if(ret == sizeof(LevelDuration)) {
            // Sample the backlog now and then (the sample just taken counts too).
            if((++instance->samples & 0x1F) == 0) {
                const size_t used =
                    furi_stream_buffer_bytes_available(instance->stream) + sizeof(LevelDuration);
                if(used > instance->stream_peak_bytes) instance->stream_peak_bytes = used;
            }
            if(level_duration_is_reset(level_duration)) {
                instance->overrun_events++;
                instance->filter_has_data = false;
                instance->filter_level_duration = level_duration_reset();
                if(instance->overrun_callback) instance->overrun_callback(instance->context);
            } else {
                bool level = level_duration_get_level(level_duration);
                uint32_t duration = level_duration_get_duration(level_duration);

                if(!instance->filter_has_data) {
                    // Store the raw bool level, like the rest of this filter does.
                    // level_duration_make() would encode it as LEVEL_DURATION_LEVEL_LOW/HIGH
                    // (1/2), so the `== level` test below would merge the first two pulses
                    // and report the first one with the wrong level.
                    instance->filter_level_duration.level = level;
                    instance->filter_level_duration.duration = duration;
                    instance->filter_has_data = true;
                    continue;
                }

                if((duration < instance->filter_duration) ||
                   (instance->filter_level_duration.level == level)) {
                    const uint32_t max_duration = (1U << 30) - 1U;
                    instance->filter_level_duration.duration =
                        (instance->filter_level_duration.duration > max_duration - duration) ?
                            max_duration :
                            instance->filter_level_duration.duration + duration;

                } else if(instance->filter_level_duration.level != level) {
                    if(instance->pair_callback)
                        instance->pair_callback(
                            instance->context,
                            instance->filter_level_duration.level,
                            instance->filter_level_duration.duration);

                    instance->filter_level_duration.duration = duration;
                    instance->filter_level_duration.level = level;
                }
            }
        }
    }

    return 0;
}

SubGhzWorker* subghz_worker_alloc(void) {
    SubGhzWorker* instance = calloc(1, sizeof(SubGhzWorker));
    if(!instance) return NULL;

    instance->thread =
        furi_thread_alloc_ex("SubGhzWorker", 8192, subghz_worker_thread_callback, instance);
    if(!instance->thread) {
        free(instance);
        return NULL;
    }
    furi_thread_set_priority(instance->thread, FuriThreadPriorityHigh);

    instance->stream =
        furi_stream_buffer_alloc_internal(
        sizeof(LevelDuration) * SUBGHZ_WORKER_STREAM_SAMPLES, sizeof(LevelDuration));
    if(!instance->stream) {
        furi_thread_free(instance->thread);
        free(instance);
        return NULL;
    }

    //setting default filter in us
    instance->filter_duration = 30;

    return instance;
}

void subghz_worker_free(SubGhzWorker* instance) {
    furi_check(instance);

    if(instance->running) {
        subghz_worker_stop(instance);
    }
    furi_stream_buffer_free(instance->stream);
    furi_thread_free(instance->thread);

    free(instance);
}

void subghz_worker_set_overrun_callback(
    SubGhzWorker* instance,
    SubGhzWorkerOverrunCallback callback) {
    furi_check(instance);
    instance->overrun_callback = callback;
}

void subghz_worker_set_pair_callback(SubGhzWorker* instance, SubGhzWorkerPairCallback callback) {
    furi_check(instance);
    instance->pair_callback = callback;
}

void subghz_worker_set_context(SubGhzWorker* instance, void* context) {
    furi_check(instance);
    instance->context = context;
}

void subghz_worker_start(SubGhzWorker* instance) {
    furi_check(instance);
    furi_check(!instance->running);

    furi_check(furi_stream_buffer_reset(instance->stream) == FuriStatusOk);
    instance->filter_has_data = false;
    instance->filter_level_duration = level_duration_reset();
    instance->overrun = false;
    instance->overrun_events = 0;
    instance->samples = 0;
    instance->stream_peak_bytes = 0;
    instance->running = true;

    furi_thread_start(instance->thread);
}

void subghz_worker_stop(SubGhzWorker* instance) {
    furi_check(instance);
    furi_check(instance->running);

    instance->running = false;

    furi_thread_join(instance->thread);

    const uint32_t peak_pct = (uint32_t)(instance->stream_peak_bytes * 100U /
                                         (SUBGHZ_WORKER_STREAM_SAMPLES * sizeof(LevelDuration)));
    if(instance->overrun_events) {
        FURI_LOG_W(
            TAG,
            "RX queue overflowed %lu times (signal dropped), %lu samples, peak %lu%%",
            instance->overrun_events,
            instance->samples,
            peak_pct);
    } else {
        FURI_LOG_D(TAG, "RX ok: %lu samples, queue peak %lu%%", instance->samples, peak_pct);
    }
}

bool subghz_worker_is_running(SubGhzWorker* instance) {
    furi_check(instance);
    return instance->running;
}

void subghz_worker_set_filter(SubGhzWorker* instance, uint16_t timeout) {
    furi_check(instance);
    instance->filter_duration = timeout;
}
