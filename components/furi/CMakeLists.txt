idf_component_register(
    SRCS
        "furi.c"
        "flipper.c"
        "core/critical.c"
        "core/check.c"
        "core/kernel.c"
        "core/log.c"
        "core/memmgr.c"
        "core/memmgr_heap.c"
        "core/thread.c"
        "core/thread_list.c"
        "core/mutex.c"
        "core/semaphore.c"
        "core/event_flag.c"
        "core/stream_buffer.c"
        "core/message_queue.c"
        "core/timer.c"
        "core/event_loop.c"
        "core/event_loop_timer.c"
        "core/event_loop_tick.c"
        "core/record.c"
        "core/pubsub.c"
        "core/furi_string.c"
    INCLUDE_DIRS "." "core"
    REQUIRES mlib toolbox
    PRIV_REQUIRES furi_hal
)

# mlib (third-party) uses strlen/memcpy/memset without #include <string.h>
# and our core/string.h shadows the system string.h, so we suppress these warnings
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=implicit-function-declaration
    -Wno-error=builtin-declaration-mismatch
)
