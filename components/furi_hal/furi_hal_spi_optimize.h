/**
 * @file furi_hal_spi_optimize.h
 * SPI Communication Optimization & Stabilization
 * 
 * Provides optimized SPI communication with:
 * - Automatic device priority handling
 * - Timing-based conflict prevention
 * - DMA optimization
 * - Error recovery with fallback frequencies
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "furi_hal_spi_stability.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SPI Operation status
 */
typedef enum {
    SpiOpStatusOk,
    SpiOpStatusTimeout,
    SpiOpStatusBusLocked,
    SpiOpStatusError,
    SpiOpStatusFrequencyFallback,
} SpiOpStatus;

/**
 * SPI communication context for optimal settings
 */
typedef struct {
    SpiDevice device;
    uint32_t frequency_hz;
    uint32_t timeout_ms;
    uint8_t retry_count;
    uint8_t dma_enabled;
    uint8_t priority;
} SpiOptimizeContext;

/**
 * Initialize SPI optimization layer
 * Must be called after furi_hal_spi_stability_init()
 */
void furi_hal_spi_optimize_init(void);

/**
 * Get optimized context for device
 * Pre-configured with proper frequency, timeout, and retry settings
 */
SpiOptimizeContext furi_hal_spi_optimize_get_context(SpiDevice device);

/**
 * Execute SPI operation with automatic:
 * - Bus locking via stability manager
 * - DMA optimization
 * - Error recovery with fallback frequency
 * - Retry logic
 * 
 * @param device       Target SPI device
 * @param operation    Function to execute (receives device config)
 * @param user_data    User context for operation
 * @return Status of operation
 */
typedef bool (*SpiOperationFn)(void* user_data);

SpiOpStatus furi_hal_spi_optimize_execute(
    SpiDevice device,
    SpiOperationFn operation,
    void* user_data
);

/**
 * Set custom frequency profile for device
 * Useful for testing/debugging
 */
void furi_hal_spi_optimize_set_frequency(SpiDevice device, uint32_t frequency_hz);

/**
 * Reset frequency to safe default
 */
void furi_hal_spi_optimize_reset_frequency(SpiDevice device);

/**
 * Enable/disable DMA for device
 */
void furi_hal_spi_optimize_set_dma(SpiDevice device, bool enabled);

/**
 * Get device statistics
 */
typedef struct {
    uint32_t operations_count;
    uint32_t operations_failed;
    uint32_t frequency_fallbacks;
    uint32_t retries_total;
    uint32_t total_lock_time_ms;
} SpiDeviceStats;

SpiDeviceStats furi_hal_spi_optimize_get_stats(SpiDevice device);

/**
 * Reset device statistics
 */
void furi_hal_spi_optimize_reset_stats(SpiDevice device);

#ifdef __cplusplus
}
#endif
