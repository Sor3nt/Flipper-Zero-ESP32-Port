#include "memmgr_heap.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>
#include <esp_heap_caps.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Thread memory tracking is not available on ESP32 port
// ESP-IDF manages its own heap

void memmgr_heap_enable_thread_trace(FuriThreadId thread_id) {
    (void)thread_id;
}

void memmgr_heap_disable_thread_trace(FuriThreadId thread_id) {
    (void)thread_id;
}

size_t memmgr_heap_get_thread_memory(FuriThreadId thread_id) {
    (void)thread_id;
    return MEMMGR_HEAP_UNKNOWN;
}

size_t memmgr_heap_get_max_free_block(void) {
    return heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
}

void memmgr_heap_printf_free_blocks(void) {
    heap_caps_print_heap_info(MALLOC_CAP_DEFAULT);
}
