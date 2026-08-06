#ifndef FURI_MEMORY_OPTIMIZE_H
#define FURI_MEMORY_OPTIMIZE_H

#include <stddef.h>
#include <stdint.h>

/**
 * Memory Pool Optimization for ESP32
 * Pre-allocate common buffers to reduce fragmentation
 */

/* Pre-allocated memory pools */
#define FURI_THREAD_STACK_MIN           512
#define FURI_THREAD_STACK_POOL_SIZE     (16 * 1024)
#define FURI_PUBSUB_SUBSCRIBERS_MAX     32

/* Heap Configuration */
#define MALLOC_CAP_DMA_POOL_SIZE        (32 * 1024)   /* Pre-allocate DMA buffer */
#define MALLOC_CAP_PSRAM_POOL_SIZE      (64 * 1024)   /* Pre-allocate PSRAM */
#define MALLOC_CAP_DEFAULT_POOL_SIZE    (64 * 1024)   /* Main heap pool */

/* GUI Memory */
#define GUI_CANVAS_BUFFER_SIZE          (256)         /* Reduced stripe buffer */
#define GUI_VIEWPORT_MAX_COUNT          8             /* Max concurrent viewports */
#define GUI_MESSAGE_QUEUE_SIZE          16            /* UI message queue */

/* Asset Loading */
#define STORAGE_CACHE_SIZE              (128 * 1024)  /* Cache frequently used files */
#define ANIMATION_FRAME_BUFFER          (256)         /* Pre-allocate animation buffer */
#define ANIMATION_PRELOAD_COUNT         3             /* Preload next animations */

/* Storage I/O */
#define FILE_IO_BUFFER_SIZE             (8 * 1024)    /* 8KB file buffer */
#define STORAGE_READ_BUFFER_SIZE        (4 * 1024)    /* 4KB read buffer */
#define STORAGE_READ_AHEAD              1             /* Enable read-ahead */

/* Memory Pool Struct */
typedef struct {
    void* dma_buffer;
    void* psram_buffer;
    void* animation_buffer;
    size_t dma_size;
    size_t psram_size;
    size_t animation_size;
    uint8_t initialized;
} MemoryPool;

/* Global memory pool (initialized at boot) */
extern MemoryPool furi_memory_pool;

/**
 * Initialize memory pools at boot
 * Must be called once during system initialization
 */
void furi_memory_pool_init(void);

/**
 * Deinitialize memory pools (cleanup)
 */
void furi_memory_pool_deinit(void);

/**
 * Get buffer from pre-allocated DMA pool
 * Returns NULL if pool exhausted or not initialized
 */
void* furi_memory_pool_get_dma(size_t size);

/**
 * Get buffer from pre-allocated PSRAM pool
 * Returns NULL if pool exhausted or not initialized
 */
void* furi_memory_pool_get_psram(size_t size);

/**
 * Get buffer from pre-allocated animation pool
 * Returns NULL if pool exhausted or not initialized
 */
void* furi_memory_pool_get_animation(size_t size);

/**
 * Release buffer back to pool
 */
void furi_memory_pool_release(void* ptr);

/**
 * Get free DMA pool size
 */
size_t furi_memory_pool_get_dma_free(void);

/**
 * Get free PSRAM pool size
 */
size_t furi_memory_pool_get_psram_free(void);

/**
 * Get free animation pool size
 */
size_t furi_memory_pool_get_animation_free(void);

#endif /* FURI_MEMORY_OPTIMIZE_H */
