/**
 * SPI Device Configuration & Error Handling
 * Safe parameters for LCD, SD, CC1101, NRF24 to prevent crashes
 */

#ifndef FURI_HAL_SPI_DEVICE_CONFIG_H
#define FURI_HAL_SPI_DEVICE_CONFIG_H

#include <stdint.h>

/* ============ LCD (ST7789) ============
 * Frequency: 40 MHz
 * Mode: SPI_MODE_0
 * Requirements: Stable, continuous transfers
 */
#define LCD_SPI_FREQ_HZ             (40 * 1000 * 1000)
#define LCD_SPI_MODE                0
#define LCD_SPI_CS_HOLD_US          2
#define LCD_SPI_CS_SETUP_US         2
#define LCD_SPI_OPERATION_TIMEOUT   100  /* 100ms for frame updates */
#define LCD_SPI_MAX_TRANSFER_SIZE   (16 * 1024)  /* 16KB per transfer */
#define LCD_SPI_DMA_ENABLED         1
#define LCD_SPI_PRIORITY            0    /* Highest */

/* ============ SD Card (SDMMC) ============
 * Frequency: 40 MHz (normal), up to 50 MHz (fast)
 * Mode: SPI_MODE_0
 * Requirements: Reliable block transfers, timeout tolerance
 */
#define SD_SPI_FREQ_HZ_NORMAL       (40 * 1000 * 1000)
#define SD_SPI_FREQ_HZ_FAST         (50 * 1000 * 1000)
#define SD_SPI_FREQ_HZ_FALLBACK     (25 * 1000 * 1000)
#define SD_SPI_MODE                 0
#define SD_SPI_CS_HOLD_US           4
#define SD_SPI_CS_SETUP_US          4
#define SD_SPI_OPERATION_TIMEOUT    5000  /* 5 seconds for block operations */
#define SD_SPI_MAX_TRANSFER_SIZE    (64 * 1024)  /* 64KB per transfer */
#define SD_SPI_DMA_ENABLED          1
#define SD_SPI_PRIORITY             1     /* High */
#define SD_SPI_RETRY_COUNT          3
#define SD_SPI_RETRY_DELAY_MS       10

/* ============ CC1101 (SubGHz) ============
 * Frequency: 10 MHz (conservative for RF stability)
 * Mode: SPI_MODE_0
 * Requirements: Strict timing, no interruption during command sequences
 */
#define CC1101_SPI_FREQ_HZ          (10 * 1000 * 1000)
#define CC1101_SPI_MODE             0
#define CC1101_SPI_CS_HOLD_US       10
#define CC1101_SPI_CS_SETUP_US      10
#define CC1101_SPI_OPERATION_TIMEOUT 500  /* 500ms for RF operations */
#define CC1101_SPI_MAX_TRANSFER_SIZE 256   /* Max packet size */
#define CC1101_SPI_DMA_ENABLED      0     /* No DMA for RF */
#define CC1101_SPI_PRIORITY         2     /* Medium */
#define CC1101_SPI_ATOMIC_OPS       1     /* Require atomic access */
#define CC1101_SPI_LOCK_REQUIRED    1

/* ============ NRF24L01 (WiFi) ============
 * Frequency: 10 MHz (RF stability)
 * Mode: SPI_MODE_0
 * Requirements: Atomic operations, no interruption
 */
#define NRF24_SPI_FREQ_HZ           (10 * 1000 * 1000)
#define NRF24_SPI_MODE              0
#define NRF24_SPI_CS_HOLD_US        10
#define NRF24_SPI_CS_SETUP_US       10
#define NRF24_SPI_OPERATION_TIMEOUT 1000  /* 1s for packet ops */
#define NRF24_SPI_MAX_TRANSFER_SIZE 256
#define NRF24_SPI_DMA_ENABLED       0
#define NRF24_SPI_PRIORITY          2
#define NRF24_SPI_ATOMIC_OPS        1
#define NRF24_SPI_LOCK_REQUIRED     1

/* ============ Safety Configuration ============ */

/* SPI Bus Arbitration */
#define SPI_BUS_LOCK_TIMEOUT_MS     100   /* Max wait for bus lock */
#define SPI_BUS_DEADLOCK_TIMEOUT_MS 5000  /* Emergency reset threshold */

/* Error Recovery */
#define SPI_ERROR_RECOVERY_ENABLED  1
#define SPI_DEVICE_RESET_ON_ERROR   1
#define SPI_BUS_RESET_ON_DEADLOCK   1

/* Monitoring */
#define SPI_MONITOR_ENABLED         1
#define SPI_MONITOR_INTERVAL_MS     100   /* Check every 100ms */
#define SPI_LOG_CONFLICTS           1
#define SPI_LOG_TIMEOUTS            1

/* Device-specific error handling */
#define LCD_ERROR_RETRY             1     /* Retry on LCD timeout */
#define LCD_ERROR_FALLBACK_FREQ     (25 * 1000 * 1000)

#define SD_ERROR_RETRY              3     /* Retry SD operations up to 3 times */
#define SD_ERROR_AUTO_RECOVER       1     /* Auto-recover from SD errors */
#define SD_ERROR_FALLBACK_FREQ      (25 * 1000 * 1000)

#define CC1101_ERROR_ABORT          1     /* Abort on CC1101 error */
#define CC1101_ERROR_RESET          1     /* Reset CC1101 on error */
#define CC1101_ERROR_RETRY          1     /* Retry RF operations */

#define NRF24_ERROR_ABORT           1
#define NRF24_ERROR_RESET           1
#define NRF24_ERROR_RETRY           1

/* ============ Conflict Prevention ============ */

/* Prevent LCD from interrupting SD operations */
#define CONFLICT_LCD_SD_PREVENT     1
#define CONFLICT_LCD_SD_PRIORITY_BOOST 1  /* Boost SD during large transfers */

/* Prevent RF operations from interrupting each other */
#define CONFLICT_RF_RF_ATOMIC       1

/* Prevent SD from interrupting RF operations (sensitive) */
#define CONFLICT_SD_RF_PREVENT      1
#define CONFLICT_SD_RF_DELAY_MS     100

/* ============ Safe Operating Modes ============ */

/* Conservative mode: safer, slower (for debug/stability testing) */
#define SAFE_MODE_CONSERVATIVE \
    { \
        .lcd_freq = (25 * 1000 * 1000), \
        .sd_freq = (20 * 1000 * 1000), \
        .cc1101_freq = (5 * 1000 * 1000), \
        .nrf24_freq = (5 * 1000 * 1000), \
        .bus_lock_timeout = 200, \
        .operation_timeout = 10000, \
        .error_retry = 5, \
    }

/* Normal mode: balanced safety and speed */
#define SAFE_MODE_NORMAL \
    { \
        .lcd_freq = (40 * 1000 * 1000), \
        .sd_freq = (40 * 1000 * 1000), \
        .cc1101_freq = (10 * 1000 * 1000), \
        .nrf24_freq = (10 * 1000 * 1000), \
        .bus_lock_timeout = 100, \
        .operation_timeout = 5000, \
        .error_retry = 3, \
    }

/* Fast mode: optimized for speed (requires stable hardware) */
#define SAFE_MODE_FAST \
    { \
        .lcd_freq = (40 * 1000 * 1000), \
        .sd_freq = (50 * 1000 * 1000), \
        .cc1101_freq = (10 * 1000 * 1000), \
        .nrf24_freq = (10 * 1000 * 1000), \
        .bus_lock_timeout = 50, \
        .operation_timeout = 3000, \
        .error_retry = 2, \
    }

#endif /* FURI_HAL_SPI_DEVICE_CONFIG_H */
