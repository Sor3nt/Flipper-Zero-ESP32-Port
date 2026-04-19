#include "memmgr.h"
#include <string.h>
#include <furi_hal_memory.h>
#include <stdlib.h>
#include <esp_heap_caps.h>

size_t memmgr_get_free_heap(void) {
    return esp_get_free_heap_size();
}

size_t memmgr_get_total_heap(void) {
    return heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
}

size_t memmgr_get_minimum_free_heap(void) {
    return esp_get_minimum_free_heap_size();
}

void* memmgr_alloc_from_pool(size_t size) {
    void* p = furi_hal_memory_alloc(size);
    if(p == NULL) p = malloc(size);
    return p;
}

size_t memmgr_pool_get_free(void) {
    return furi_hal_memory_get_free();
}

size_t memmgr_pool_get_max_block(void) {
    return furi_hal_memory_max_pool_block();
}

void* aligned_malloc(size_t size, size_t alignment) {
    void* p1;
    void** p2;
    int offset = alignment - 1 + sizeof(void*);
    if((p1 = (void*)malloc(size + offset)) == NULL) {
        return NULL;
    }
    p2 = (void**)(((size_t)(p1) + offset) & ~(alignment - 1));
    p2[-1] = p1;
    return p2;
}

void aligned_free(void* p) {
    if(p) {
        free(((void**)p)[-1]);
    }
}
