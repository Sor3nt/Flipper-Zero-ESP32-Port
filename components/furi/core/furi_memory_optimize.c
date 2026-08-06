/**
 * Memory Pool Optimization Implementation for ESP32
 * Pre-allocate common buffers to reduce fragmentation
 */

#include "furi_memory_optimize.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdlib.h>
#include <string.h>

static const char* TAG = "FuriMemPool";

/* Global memory pool instance */
MemoryPool furi_memory_pool = {
    .dma_buffer = NULL,
    .psram_buffer = NULL,
    .animation_buffer = NULL,
    .dma_size = 0,
    .psram_size = 0,
    .animation_size = 0,
    .initialized = 0
};

/* Memory pool tracking */
typedef struct {
    void* buffer;
    size_t size;
    uint8_t in_use;
} PoolBuffer;

static PoolBuffer pool_dma = {NULL, 0, 0};
static PoolBuffer pool_psram = {NULL, 0, 0};
static PoolBuffer pool_animation = {NULL, 0, 0};
static StaticSemaphore_t pool_mutex_buffer;
static SemaphoreHandle_t pool_mutex = NULL;

static void pool_lock(void) {
    if (pool_mutex) {
        xSemaphoreTake(pool_mutex, portMAX_DELAY);
    }
}

static void pool_unlock(void) {
    if (pool_mutex) {
        xSemaphoreGive(pool_mutex);
    }
}

void furi_memory_pool_init(void) {
    if (furi_memory_pool.initialized) {
        ESP_LOGD(TAG, "Memory pool already initialized");
        return;
    }

    ESP_LOGI(TAG, "Initializing memory pools");

    /* Create mutex for thread-safe pool access */
    pool_mutex = xSemaphoreCreateMutexStatic(&pool_mutex_buffer);
    if (!pool_mutex) {
        ESP_LOGE(TAG, "Failed to create pool mutex");
        return;
    }

    pool_lock();

    /* Allocate DMA buffer (32KB) - try DMA-capable memory first */
    furi_memory_pool.dma_size = MALLOC_CAP_DMA_POOL_SIZE;
    furi_memory_pool.dma_buffer = heap_caps_malloc(furi_memory_pool.dma_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!furi_memory_pool.dma_buffer) {
        /* Fallback to regular internal RAM */
        furi_memory_pool.dma_buffer = malloc(furi_memory_pool.dma_size);
    }
    if (furi_memory_pool.dma_buffer) {
        pool_dma.buffer = furi_memory_pool.dma_buffer;
        pool_dma.size = furi_memory_pool.dma_size;
        pool_dma.in_use = 0;
        ESP_LOGI(TAG, "DMA pool allocated: %u bytes", furi_memory_pool.dma_size);
    } else {
        ESP_LOGW(TAG, "Failed to allocate DMA pool");
        furi_memory_pool.dma_size = 0;
    }

    /* Allocate PSRAM buffer (64KB) */
    furi_memory_pool.psram_size = MALLOC_CAP_PSRAM_POOL_SIZE;
    furi_memory_pool.psram_buffer = heap_caps_malloc(furi_memory_pool.psram_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!furi_memory_pool.psram_buffer) {
        /* Fallback to internal RAM if PSRAM not available */
        furi_memory_pool.psram_buffer = malloc(furi_memory_pool.psram_size);
    }
    if (furi_memory_pool.psram_buffer) {
        pool_psram.buffer = furi_memory_pool.psram_buffer;
        pool_psram.size = furi_memory_pool.psram_size;
        pool_psram.in_use = 0;
        ESP_LOGI(TAG, "PSRAM pool allocated: %u bytes", furi_memory_pool.psram_size);
    } else {
        ESP_LOGW(TAG, "Failed to allocate PSRAM pool");
        furi_memory_pool.psram_size = 0;
    }

    /* Allocate animation buffer (256 bytes) */
    furi_memory_pool.animation_size = ANIMATION_FRAME_BUFFER;
    furi_memory_pool.animation_buffer = malloc(furi_memory_pool.animation_size);
    if (furi_memory_pool.animation_buffer) {
        pool_animation.buffer = furi_memory_pool.animation_buffer;
        pool_animation.size = furi_memory_pool.animation_size;
        pool_animation.in_use = 0;
        ESP_LOGI(TAG, "Animation pool allocated: %u bytes", furi_memory_pool.animation_size);
    } else {
        ESP_LOGW(TAG, "Failed to allocate animation pool");
        furi_memory_pool.animation_size = 0;
    }

    furi_memory_pool.initialized = 1;
    pool_unlock();
    ESP_LOGI(TAG, "Memory pools initialized successfully");
}

void* furi_memory_pool_get_dma(size_t size) {
    if (!furi_memory_pool.initialized) {
        ESP_LOGE(TAG, "Memory pools not initialized");
        return NULL;
    }

    pool_lock();

    if (!pool_dma.buffer || pool_dma.in_use) {
        ESP_LOGD(TAG, "DMA pool not available (in use or allocation failed)");
        pool_unlock();
        return NULL;
    }

    if (size > pool_dma.size) {
        ESP_LOGW(TAG, "DMA pool too small: requested %u, available %u", size, pool_dma.size);
        pool_unlock();
        return NULL;
    }

    pool_dma.in_use = 1;
    void* ptr = pool_dma.buffer;
    pool_unlock();
    ESP_LOGV(TAG, "DMA pool allocated: %u bytes", size);
    return ptr;
}

void* furi_memory_pool_get_psram(size_t size) {
    if (!furi_memory_pool.initialized) {
        ESP_LOGE(TAG, "Memory pools not initialized");
        return NULL;
    }

    pool_lock();

    if (!pool_psram.buffer || pool_psram.in_use) {
        ESP_LOGD(TAG, "PSRAM pool not available (in use or allocation failed)");
        pool_unlock();
        return NULL;
    }

    if (size > pool_psram.size) {
        ESP_LOGW(TAG, "PSRAM pool too small: requested %u, available %u", size, pool_psram.size);
        pool_unlock();
        return NULL;
    }

    pool_psram.in_use = 1;
    void* ptr = pool_psram.buffer;
    pool_unlock();
    ESP_LOGV(TAG, "PSRAM pool allocated: %u bytes", size);
    return ptr;
}

void* furi_memory_pool_get_animation(size_t size) {
    if (!furi_memory_pool.initialized) {
        ESP_LOGE(TAG, "Memory pools not initialized");
        return NULL;
    }

    pool_lock();

    if (!pool_animation.buffer || pool_animation.in_use) {
        ESP_LOGD(TAG, "Animation pool not available (in use or allocation failed)");
        pool_unlock();
        return NULL;
    }

    if (size > pool_animation.size) {
        ESP_LOGW(TAG, "Animation pool too small: requested %u, available %u", size, pool_animation.size);
        pool_unlock();
        return NULL;
    }

    pool_animation.in_use = 1;
    void* ptr = pool_animation.buffer;
    pool_unlock();
    ESP_LOGV(TAG, "Animation pool allocated: %u bytes", size);
    return ptr;
}

void furi_memory_pool_release(void* ptr) {
    if (!ptr) return;

    pool_lock();

    /* Check which pool this pointer belongs to and release it */
    if (ptr == pool_dma.buffer) {
        pool_dma.in_use = 0;
        pool_unlock();
        ESP_LOGV(TAG, "DMA pool released");
        return;
    }

    if (ptr == pool_psram.buffer) {
        pool_psram.in_use = 0;
        pool_unlock();
        ESP_LOGV(TAG, "PSRAM pool released");
        return;
    }

    if (ptr == pool_animation.buffer) {
        pool_animation.in_use = 0;
        pool_unlock();
        ESP_LOGV(TAG, "Animation pool released");
        return;
    }

    pool_unlock();
    ESP_LOGW(TAG, "Attempted to release unrecognized pool buffer");
}

void furi_memory_pool_deinit(void) {
    if (!furi_memory_pool.initialized) return;

    ESP_LOGI(TAG, "Deinitializing memory pools");

    pool_lock();

    if (furi_memory_pool.dma_buffer) {
        free(furi_memory_pool.dma_buffer);
        furi_memory_pool.dma_buffer = NULL;
        pool_dma.buffer = NULL;
    }

    if (furi_memory_pool.psram_buffer) {
        free(furi_memory_pool.psram_buffer);
        furi_memory_pool.psram_buffer = NULL;
        pool_psram.buffer = NULL;
    }

    if (furi_memory_pool.animation_buffer) {
        free(furi_memory_pool.animation_buffer);
        furi_memory_pool.animation_buffer = NULL;
        pool_animation.buffer = NULL;
    }

    furi_memory_pool.initialized = 0;
    pool_unlock();

    if (pool_mutex) {
        vSemaphoreDelete(pool_mutex);
        pool_mutex = NULL;
    }

    ESP_LOGI(TAG, "Memory pools deinitialized");
}

/* Get pool statistics */
size_t furi_memory_pool_get_dma_free(void) {
    size_t free_size = 0;
    pool_lock();
    if (!pool_dma.in_use && pool_dma.buffer) {
        free_size = pool_dma.size;
    }
    pool_unlock();
    return free_size;
}

size_t furi_memory_pool_get_psram_free(void) {
    size_t free_size = 0;
    pool_lock();
    if (!pool_psram.in_use && pool_psram.buffer) {
        free_size = pool_psram.size;
    }
    pool_unlock();
    return free_size;
}

size_t furi_memory_pool_get_animation_free(void) {
    size_t free_size = 0;
    pool_lock();
    if (!pool_animation.in_use && pool_animation.buffer) {
        free_size = pool_animation.size;
    }
    pool_unlock();
    return free_size;
}
