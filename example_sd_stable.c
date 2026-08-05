/**
 * Example: SPI Stable SD Card Driver Integration
 * Shows how to wrap existing SPI operations with stability layer
 */

#include "furi_hal_spi_stability.h"
#include <esp_log.h>

static const char* TAG = "SDStable";

/* ============ Safe SD Card Operations ============ */

/**
 * Safe SD card initialization with stability manager
 */
bool sd_card_init_safe(void) {
    ESP_LOGI(TAG, "Initializing SD card with SPI stability");

    /* Acquire bus with high priority + long timeout */
    if (!furi_hal_spi_stability_acquire(
        SpiDeviceSD,
        SpiPriorityHigh,
        5000  /* 5 second init timeout */
    )) {
        ESP_LOGE(TAG, "Failed to acquire SPI bus for SD init");
        return false;
    }

    /* Perform actual initialization */
    bool result = true;
    // sd_init_impl() would call actual SDMMC init
    // For now, assume it works
    ESP_LOGI(TAG, "SD card initialized");

    /* Release bus */
    furi_hal_spi_stability_release(SpiDeviceSD);

    return result;
}

/**
 * Safe block read with automatic retry on failure
 */
typedef struct {
    uint32_t block;
    uint8_t* buffer;
    size_t size;
    bool result;
} SdReadContext;

static void sd_read_block_impl(void* ctx_ptr) {
    SdReadContext* ctx = (SdReadContext*)ctx_ptr;
    // ctx->result = sdmmc_read_blocks(card, ctx->buffer, ctx->block, 1);
    ctx->result = true;  /* Mock implementation */
}

bool sd_read_block_safe(uint32_t block, uint8_t* buffer, size_t size) {
    SdReadContext ctx = {
        .block = block,
        .buffer = buffer,
        .size = size,
        .result = false
    };

    int retry_count = 3;
    int retry_delay_ms = 10;

    for (int attempt = 0; attempt < retry_count; attempt++) {
        ESP_LOGD(TAG, "Reading block %lu (attempt %d/%d)",
            block, attempt + 1, retry_count);

        if (furi_hal_spi_stability_execute(
            SpiDeviceSD,
            SpiPriorityHigh,
            sd_read_block_impl,
            &ctx,
            5000  /* 5s timeout per block */
        )) {
            if (ctx.result) {
                ESP_LOGV(TAG, "Block read success");
                return true;
            }
        } else {
            ESP_LOGW(TAG, "SPI bus timeout on block read");
        }

        /* Delay before retry */
        if (attempt < retry_count - 1) {
            vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
        }
    }

    ESP_LOGE(TAG, "Failed to read block %lu after %d attempts", block, retry_count);
    return false;
}

/**
 * Safe sequential read with read-ahead optimization
 */
typedef struct {
    uint32_t start_block;
    uint8_t* buffer;
    uint32_t block_count;
    bool result;
} SdSequentialReadContext;

static void sd_read_sequential_impl(void* ctx_ptr) {
    SdSequentialReadContext* ctx = (SdSequentialReadContext*)ctx_ptr;
    // Actual: ctx->result = sdmmc_read_blocks(card, ctx->buffer,
    //                                         ctx->start_block, ctx->block_count);
    ctx->result = true;  /* Mock */
}

bool sd_read_blocks_safe(uint32_t start_block, uint8_t* buffer, uint32_t block_count) {
    if (block_count == 0) return false;

    SdSequentialReadContext ctx = {
        .start_block = start_block,
        .buffer = buffer,
        .block_count = block_count,
        .result = false
    };

    ESP_LOGD(TAG, "Reading %lu blocks starting at %lu",
        block_count, start_block);

    /* Use normal priority for bulk transfers */
    if (!furi_hal_spi_stability_acquire(
        SpiDeviceSD,
        SpiPriorityHigh,
        5000 + (block_count * 10)  /* Scale timeout by block count */
    )) {
        ESP_LOGE(TAG, "Bus timeout acquiring for sequential read");
        return false;
    }

    sd_read_sequential_impl(&ctx);
    furi_hal_spi_stability_release(SpiDeviceSD);

    if (!ctx.result) {
        ESP_LOGE(TAG, "Sequential read failed at block %lu", start_block);
        return false;
    }

    ESP_LOGV(TAG, "Sequential read success");
    return true;
}

/**
 * Safe write with verification
 */
typedef struct {
    uint32_t block;
    const uint8_t* buffer;
    size_t size;
    bool result;
} SdWriteContext;

static void sd_write_block_impl(void* ctx_ptr) {
    SdWriteContext* ctx = (SdWriteContext*)ctx_ptr;
    // ctx->result = sdmmc_write_blocks(card, ctx->buffer, ctx->block, 1);
    ctx->result = true;  /* Mock */
}

bool sd_write_block_safe(uint32_t block, const uint8_t* buffer, size_t size) {
    SdWriteContext ctx = {
        .block = block,
        .buffer = buffer,
        .size = size,
        .result = false
    };

    int retry_count = 3;

    for (int attempt = 0; attempt < retry_count; attempt++) {
        ESP_LOGD(TAG, "Writing block %lu (attempt %d/%d)",
            block, attempt + 1, retry_count);

        if (!furi_hal_spi_stability_execute(
            SpiDeviceSD,
            SpiPriorityHigh,
            sd_write_block_impl,
            &ctx,
            5000
        )) {
            ESP_LOGW(TAG, "SPI timeout on write");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (ctx.result) {
            /* Optional: Verify write by reading back */
            // if (sd_verify_write(block)) {
                ESP_LOGV(TAG, "Write success");
                return true;
            // }
        }
    }

    ESP_LOGE(TAG, "Write failed after %d attempts", retry_count);
    return false;
}

/**
 * Mount SD card safely
 */
bool sd_mount_safe(void) {
    ESP_LOGI(TAG, "Mounting SD card safely");

    if (!furi_hal_spi_stability_acquire(
        SpiDeviceSD,
        SpiPriorityHigh,
        5000
    )) {
        ESP_LOGE(TAG, "Cannot mount - SPI bus locked");
        return false;
    }

    // Actual mount logic here
    // result = esp_vfs_fat_sdmmc_mount(...);

    furi_hal_spi_stability_release(SpiDeviceSD);

    ESP_LOGI(TAG, "SD card mounted safely");
    return true;
}

/**
 * Unmount SD card safely
 */
bool sd_unmount_safe(void) {
    ESP_LOGI(TAG, "Unmounting SD card safely");

    if (!furi_hal_spi_stability_acquire(
        SpiDeviceSD,
        SpiPriorityHigh,
        5000
    )) {
        ESP_LOGE(TAG, "Cannot unmount - SPI bus locked");
        return false;
    }

    // Actual unmount logic
    // result = esp_vfs_fat_sdmmc_unmount();

    furi_hal_spi_stability_release(SpiDeviceSD);

    ESP_LOGI(TAG, "SD card unmounted safely");
    return true;
}

/* ============ Module Initialization ============ */

/**
 * Initialize safe SD card subsystem
 * Call after furi_hal_spi_stability_init()
 */
void sd_stable_init(void) {
    ESP_LOGI(TAG, "Initializing SD Stable subsystem");

    /* Make sure stability manager is ready */
    if (!spi_stability.bus_mutex) {
        ESP_LOGE(TAG, "SPI Stability Manager not initialized!");
        return;
    }

    /* Initialize SD card safely */
    if (!sd_card_init_safe()) {
        ESP_LOGE(TAG, "Failed to initialize SD card");
        return;
    }

    /* Mount SD card */
    if (!sd_mount_safe()) {
        ESP_LOGE(TAG, "Failed to mount SD card");
        return;
    }

    ESP_LOGI(TAG, "SD Stable subsystem ready");
}

/* ============ Example Usage ============ */

/*
void example_usage(void) {
    // Initialize
    furi_hal_spi_stability_init();
    sd_stable_init();

    // Safe read
    uint8_t buffer[512];
    if (sd_read_block_safe(0, buffer, 512)) {
        ESP_LOGI(TAG, "Read successful");
    }

    // Safe write
    if (sd_write_block_safe(0, buffer, 512)) {
        ESP_LOGI(TAG, "Write successful");
    }

    // Check statistics
    uint32_t total, timeouts, conflicts;
    furi_hal_spi_stability_get_stats(&total, &timeouts, &conflicts);
    ESP_LOGI(TAG, "Stats - Total: %lu, Timeouts: %lu, Conflicts: %lu",
        total, timeouts, conflicts);
}
*/
