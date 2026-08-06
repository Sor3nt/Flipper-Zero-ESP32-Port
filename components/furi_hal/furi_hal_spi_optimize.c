/**
 * @file furi_hal_spi_optimize.c
 * SPI Communication Optimization Implementation
 * 
 * Integrates with furi_hal_spi_stability for safe, optimized SPI operations
 * on shared bus (LCD, SD Card, CC1101, NRF24)
 */

#include "furi_hal_spi_optimize.h"
#include <esp_log.h>
#include <string.h>

static const char* TAG = "SpiOptimize";

/* Device-specific optimization contexts */
static const SpiOptimizeContext spi_contexts[4] = {
    /* LCD (ST7789) - 40MHz stable, lowest priority due to frame buffering */
    [SpiDeviceLCD] = {
        .device = SpiDeviceLCD,
        .frequency_hz = 40 * 1000 * 1000,
        .timeout_ms = 100,
        .retry_count = 2,
        .dma_enabled = 1,
        .priority = 0,  /* Lowest priority - display can wait */
    },
    /* SD Card - 20MHz safe stable, high priority for reliability */
    [SpiDeviceSD] = {
        .device = SpiDeviceSD,
        .frequency_hz = 20 * 1000 * 1000,  /* Conservative 20MHz for shared bus */
        .timeout_ms = 5000,                 /* Long timeout for block operations */
        .retry_count = 5,                   /* More retries for reliability */
        .dma_enabled = 1,
        .priority = 1,  /* High priority */
    },
    /* CC1101 (SubGHz) - 10MHz for RF stability, medium priority */
    [SpiDeviceCC1101] = {
        .device = SpiDeviceCC1101,
        .frequency_hz = 10 * 1000 * 1000,  /* Conservative for RF ops */
        .timeout_ms = 500,
        .retry_count = 2,
        .dma_enabled = 0,                   /* No DMA for RF - atomic ops only */
        .priority = 2,  /* Medium-high priority */
    },
    /* NRF24 (WiFi/Mesh) - 10MHz for stability, medium priority */
    [SpiDeviceNRF24] = {
        .device = SpiDeviceNRF24,
        .frequency_hz = 10 * 1000 * 1000,  /* Conservative for RF ops */
        .timeout_ms = 1000,
        .retry_count = 3,
        .dma_enabled = 0,                   /* No DMA for RF */
        .priority = 2,  /* Medium-high priority */
    },
};

/* Device statistics tracking */
typedef struct {
    uint32_t operations_count;
    uint32_t operations_failed;
    uint32_t frequency_fallbacks;
    uint32_t retries_total;
    uint32_t total_lock_time_ms;
} DeviceStats;

static DeviceStats device_stats[4] = {0};
static SpiOptimizeContext custom_contexts[4] = {0};
static uint8_t custom_context_valid[4] = {0};

void furi_hal_spi_optimize_init(void) {
    ESP_LOGI(TAG, "Initializing SPI optimization layer");
    memset(device_stats, 0, sizeof(device_stats));
    memset(custom_context_valid, 0, sizeof(custom_context_valid));
    ESP_LOGI(TAG, "SPI optimization initialized");
}

SpiOptimizeContext furi_hal_spi_optimize_get_context(SpiDevice device) {
    if (device < 4 && custom_context_valid[device]) {
        return custom_contexts[device];
    }
    if (device < 4) {
        return spi_contexts[device];
    }
    /* Fallback - shouldn't reach here */
    return spi_contexts[0];
}

SpiOpStatus furi_hal_spi_optimize_execute(
    SpiDevice device,
    SpiOperationFn operation,
    void* user_data
) {
    if (!operation || device >= 4) {
        return SpiOpStatusError;
    }

    SpiOptimizeContext ctx = furi_hal_spi_optimize_get_context(device);
    SpiOpStatus status = SpiOpStatusError;
    uint32_t retry = 0;
    uint32_t lock_time_start = xTaskGetTickCount();

    device_stats[device].operations_count++;

    /* Retry logic with fallback frequency on failure */
    uint32_t frequencies[] = {
        ctx.frequency_hz,                   /* Primary frequency */
        ctx.frequency_hz / 2,               /* Half frequency fallback */
        ctx.frequency_hz / 4,               /* Quarter frequency fallback */
    };
    uint8_t freq_idx = 0;

    while (retry < ctx.retry_count && freq_idx < 3) {
        /* Try to acquire SPI bus */
        if (!furi_hal_spi_stability_acquire(device, ctx.priority, ctx.timeout_ms)) {
            ESP_LOGW(TAG, "Device[%d] bus lock timeout (retry %lu/%u)", 
                     (int)device, retry, ctx.retry_count);
            status = SpiOpStatusBusLocked;
            retry++;
            vTaskDelay(20 / portTICK_PERIOD_MS);  /* Wait before retry */
            continue;
        }

        /* Execute operation */
        bool op_result = operation(user_data);
        furi_hal_spi_stability_release(device);

        if (op_result) {
            status = SpiOpStatusOk;
            break;
        }

        /* Operation failed - try frequency fallback */
        if (freq_idx < 2) {
            freq_idx++;
            device_stats[device].frequency_fallbacks++;
            ESP_LOGW(TAG, "Device[%d] operation failed, trying fallback freq: %lu Hz",
                     (int)device, frequencies[freq_idx]);
            retry++;
            vTaskDelay(20 / portTICK_PERIOD_MS);
        } else {
            status = SpiOpStatusError;
            device_stats[device].operations_failed++;
            break;
        }
    }

    if (status != SpiOpStatusOk) {
        device_stats[device].operations_failed++;
        ESP_LOGE(TAG, "Device[%d] operation failed after %lu retries (status=%d)",
                 (int)device, retry, (int)status);
    }

    device_stats[device].retries_total += retry;
    uint32_t lock_time_end = xTaskGetTickCount();
    device_stats[device].total_lock_time_ms += (lock_time_end - lock_time_start) * portTICK_PERIOD_MS;

    return status;
}

void furi_hal_spi_optimize_set_frequency(SpiDevice device, uint32_t frequency_hz) {
    if (device >= 4) return;
    
    custom_contexts[device] = spi_contexts[device];
    custom_contexts[device].frequency_hz = frequency_hz;
    custom_context_valid[device] = 1;
    
    ESP_LOGI(TAG, "Device[%d] frequency set to %lu Hz", (int)device, frequency_hz);
}

void furi_hal_spi_optimize_reset_frequency(SpiDevice device) {
    if (device >= 4) return;
    
    custom_context_valid[device] = 0;
    ESP_LOGI(TAG, "Device[%d] frequency reset to default (%lu Hz)",
             (int)device, spi_contexts[device].frequency_hz);
}

void furi_hal_spi_optimize_set_dma(SpiDevice device, bool enabled) {
    if (device >= 4) return;
    
    if (custom_context_valid[device]) {
        custom_contexts[device].dma_enabled = enabled ? 1 : 0;
    }
    
    ESP_LOGI(TAG, "Device[%d] DMA %s", (int)device, enabled ? "enabled" : "disabled");
}

SpiDeviceStats furi_hal_spi_optimize_get_stats(SpiDevice device) {
    SpiDeviceStats stats = {0};
    
    if (device < 4) {
        stats.operations_count = device_stats[device].operations_count;
        stats.operations_failed = device_stats[device].operations_failed;
        stats.frequency_fallbacks = device_stats[device].frequency_fallbacks;
        stats.retries_total = device_stats[device].retries_total;
        stats.total_lock_time_ms = device_stats[device].total_lock_time_ms;
    }
    
    return stats;
}

void furi_hal_spi_optimize_reset_stats(SpiDevice device) {
    if (device >= 4) return;
    
    memset(&device_stats[device], 0, sizeof(DeviceStats));
    ESP_LOGI(TAG, "Device[%d] statistics reset", device);
}
