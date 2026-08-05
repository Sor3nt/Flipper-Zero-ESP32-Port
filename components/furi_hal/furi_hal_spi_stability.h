/**
 * SPI Bus Stability & Arbitration Layer
 *
 * Manages concurrent access from:
 * - LCD (Display) - SPI2
 * - SD Card - SPI2
 * - CC1101 (SubGHz) - SPI2
 * - NRF24L01 (WiFi) - SPI2
 *
 * Prevents conflicts, deadlocks, and crashes
 */

#ifndef FURI_HAL_SPI_STABILITY_H
#define FURI_HAL_SPI_STABILITY_H

#include <stdint.h>
#include <stdbool.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/* SPI Device Types */
typedef enum {
    SpiDeviceLCD = 0,      /* Display - highest priority */
    SpiDeviceSD = 1,       /* SD card - high priority */
    SpiDeviceCC1101 = 2,   /* SubGHz - medium priority */
    SpiDeviceNRF24 = 3,    /* WiFi - medium priority */
    SpiDeviceMAX = 4,
} SpiDevice;

/* SPI Operation Priorities */
typedef enum {
    SpiPriorityUrgent = 0,   /* Display update, critical I/O */
    SpiPriorityHigh = 1,     /* Normal operations */
    SpiPriorityNormal = 2,   /* Background operations */
    SpiPriorityLow = 3,      /* Scanning, monitoring */
} SpiPriority;

/* SPI Bus State */
typedef struct {
    /* Ownership tracking */
    SpiDevice current_owner;
    uint32_t owner_tid;           /* Owner thread ID */

    /* Synchronization */
    SemaphoreHandle_t bus_mutex;  /* Binary semaphore - bus lock */
    SemaphoreHandle_t bus_ready;  /* Signaling semaphore - operation done */

    /* Timeout management */
    uint32_t operation_start_ms;
    uint32_t operation_timeout_ms;

    /* Priority queue */
    uint8_t priority_queue[SpiDeviceMAX];  /* Waiting devices by priority */
    uint8_t queue_head;
    uint8_t queue_tail;
    uint8_t queue_count;
    uint8_t device_priority[SpiDeviceMAX];   /* Priority for each device */

    /* Statistics & monitoring */
    uint32_t total_operations;
    uint32_t timeout_count;
    uint32_t conflict_count;
    uint32_t retry_count;
    uint32_t last_error;

    /* Device status */
    uint8_t device_active[SpiDeviceMAX];
    uint32_t device_last_access_ms[SpiDeviceMAX];

} SpiStabilityManager;

/* Global SPI stability manager */
extern SpiStabilityManager spi_stability;

/**
 * Initialize SPI stability manager
 * Call once at system boot, before any SPI operations
 */
void furi_hal_spi_stability_init(void);

/**
 * Acquire SPI bus lock for a device
 *
 * @param device: Device requesting access
 * @param priority: Operation priority level
 * @param timeout_ms: Max wait time (0 = no wait, -1 = infinite)
 * @return: true if lock acquired, false if timeout
 *
 * Example:
 *   if (furi_hal_spi_stability_acquire(SpiDeviceSD, SpiPriorityHigh, 100)) {
 *       // Do SPI operation
 *       furi_hal_spi_stability_release(SpiDeviceSD);
 *   }
 */
bool furi_hal_spi_stability_acquire(SpiDevice device, SpiPriority priority, uint32_t timeout_ms);

/**
 * Release SPI bus lock
 * Must be called after every acquire() call
 */
void furi_hal_spi_stability_release(SpiDevice device);

/**
 * Atomic SPI operation (acquire + operation + release)
 * For simple, short operations
 */
bool furi_hal_spi_stability_execute(
    SpiDevice device,
    SpiPriority priority,
    void (*operation)(void* ctx),
    void* context,
    uint32_t timeout_ms
);

/**
 * Check if SPI bus is busy
 */
bool furi_hal_spi_stability_is_busy(void);

/**
 * Get current SPI bus owner
 */
SpiDevice furi_hal_spi_stability_get_owner(void);

/**
 * Emergency SPI bus reset (use only if deadlock detected)
 */
void furi_hal_spi_stability_emergency_reset(void);

/**
 * Get SPI stability statistics
 */
void furi_hal_spi_stability_get_stats(
    uint32_t* total_ops,
    uint32_t* timeouts,
    uint32_t* conflicts
);

/**
 * Detect and recover from SPI deadlocks
 * Call periodically (e.g., from watchdog)
 */
bool furi_hal_spi_stability_detect_deadlock(void);

/**
 * Configure device-specific SPI parameters
 */
typedef struct {
    uint32_t clock_hz;
    uint8_t cs_hold_us;
    uint8_t cs_setup_us;
    uint16_t timeout_ms;
    uint8_t priority;
} SpiDeviceConfig;

void furi_hal_spi_stability_set_device_config(
    SpiDevice device,
    const SpiDeviceConfig* config
);

/**
 * Enable device-specific error recovery
 */
void furi_hal_spi_stability_enable_recovery(SpiDevice device, bool enabled);

/* Default timeout values for each device */
#define SPI_TIMEOUT_LCD_MS          100   /* 100ms for display frames */
#define SPI_TIMEOUT_SD_MS           5000  /* 5s for block operations */
#define SPI_TIMEOUT_CC1101_MS       500   /* 500ms for RF operations */
#define SPI_TIMEOUT_NRF24_MS        1000  /* 1s for packet operations */

/* Retry configuration */
#define SPI_RETRY_COUNT_DEFAULT     3
#define SPI_RETRY_DELAY_MS          10

#endif /* FURI_HAL_SPI_STABILITY_H */