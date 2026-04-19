#include "furi_hal_memory.h"
#include <stdlib.h>

void furi_hal_memory_init(void) {
}

void* furi_hal_memory_alloc(size_t size) {
    // No separate memory pool on ESP32, fall back to regular malloc
    (void)size;
    return NULL;
}

size_t furi_hal_memory_get_free(void) {
    return 0;
}

size_t furi_hal_memory_max_pool_block(void) {
    return 0;
}
