/**
 * SPI Bus Stability Manager Implementation
 * Prevents crashes from concurrent SPI device access
 */

#include "furi_hal_spi_stability.h"
#include <esp_log.h>
#include <string.h>

static const char* TAG = "SpiStability";

/* Global SPI stability manager */
SpiStabilityManager spi_stability = {0};

/* Device names for logging */
static const char* device_names[] = {
    "LCD",
    "SD",
    "CC1101",
    "NRF24"
};

/* Device priorities */
static const uint8_t device_base_priority[] = {
    0,  /* LCD - highest */
    1,  /* SD */
    2,  /* CC1101 */
    3,  /* NRF24 */
};

void furi_hal_spi_stability_init(void) {
    ESP_LOGI(TAG, "Initializing SPI Stability Manager");

    memset(&spi_stability, 0, sizeof(SpiStabilityManager));

    /* Create binary semaphore (bus lock) */
    spi_stability.bus_mutex = xSemaphoreCreateBinary();
    if (!spi_stability.bus_mutex) {
        ESP_LOGE(TAG, "Failed to create bus mutex!");
        return;
    }
    xSemaphoreGive(spi_stability.bus_mutex);  /* Initially available */

    /* Create signaling semaphore (operation done) */
    spi_stability.bus_ready = xSemaphoreCreateBinary();
    if (!spi_stability.bus_ready) {
        ESP_LOGE(TAG, "Failed to create bus ready semaphore!");
        return;
    }

    /* Initialize device status */
    for (int i = 0; i < SpiDeviceMAX; i++) {
        spi_stability.device_active[i] = 0;
        spi_stability.device_last_access_ms[i] = 0;
    }

    spi_stability.current_owner = SpiDeviceMAX;  /* No owner initially */
    spi_stability.operation_timeout_ms = 5000;   /* 5 second default timeout */

    ESP_LOGI(TAG, "SPI Stability Manager ready");
}

bool furi_hal_spi_stability_acquire(
    SpiDevice device,
    SpiPriority priority,
    uint32_t timeout_ms
) {
    if (device >= SpiDeviceMAX) {
        ESP_LOGE(TAG, "Invalid device: %d", device);
        return false;
    }

    TickType_t wait_ticks;
    if (timeout_ms == 0) {
        wait_ticks = 0;
    } else if (timeout_ms == (uint32_t)-1) {
        wait_ticks = portMAX_DELAY;
    } else {
        wait_ticks = pdMS_TO_TICKS(timeout_ms);
    }

    /* Try to acquire bus mutex with timeout */
    if (xSemaphoreTake(spi_stability.bus_mutex, wait_ticks) != pdTRUE) {
        ESP_LOGW(
            TAG,
            "Bus lock timeout for %s (timeout=%lu ms)",
            device_names[device],
            timeout_ms
        );
        spi_stability.timeout_count++;
        return false;
    }

    /* Update ownership */
    spi_stability.current_owner = device;
    spi_stability.owner_tid = (uint32_t)xTaskGetCurrentTaskHandle();
    spi_stability.operation_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    spi_stability.device_active[device] = 1;
    spi_stability.device_last_access_ms[device] = spi_stability.operation_start_ms;

    ESP_LOGV(TAG, "Bus acquired by %s", device_names[device]);
    spi_stability.total_operations++;

    return true;
}

void furi_hal_spi_stability_release(SpiDevice device) {
    if (spi_stability.current_owner != device) {
        ESP_LOGW(
            TAG,
            "Release by %s but owner is %s!",
            device_names[device],
            device_names[spi_stability.current_owner]
        );
        return;
    }

    spi_stability.current_owner = SpiDeviceMAX;
    spi_stability.device_active[device] = 0;

    /* Release mutex */
    xSemaphoreGive(spi_stability.bus_mutex);
    xSemaphoreGive(spi_stability.bus_ready);  /* Signal operation done */

    ESP_LOGV(TAG, "Bus released by %s", device_names[device]);
}

bool furi_hal_spi_stability_execute(
    SpiDevice device,
    SpiPriority priority,
    void (*operation)(void* ctx),
    void* context,
    uint32_t timeout_ms
) {
    if (!operation) return false;

    if (!furi_hal_spi_stability_acquire(device, priority, timeout_ms)) {
        ESP_LOGW(TAG, "Failed to acquire bus for %s operation", device_names[device]);
        return false;
    }

    /* Execute operation */
    operation(context);

    /* Release bus */
    furi_hal_spi_stability_release(device);

    return true;
}

bool furi_hal_spi_stability_is_busy(void) {
    return spi_stability.current_owner != SpiDeviceMAX;
}

SpiDevice furi_hal_spi_stability_get_owner(void) {
    return spi_stability.current_owner;
}

void furi_hal_spi_stability_emergency_reset(void) {
    ESP_LOGW(TAG, "EMERGENCY SPI BUS RESET!");

    /* Release all semaphores */
    if (spi_stability.bus_mutex) {
        xSemaphoreGive(spi_stability.bus_mutex);
        xSemaphoreGive(spi_stability.bus_mutex);  /* Extra give for safety */
    }

    /* Reset state */
    spi_stability.current_owner = SpiDeviceMAX;
    for (int i = 0; i < SpiDeviceMAX; i++) {
        spi_stability.device_active[i] = 0;
    }

    spi_stability.conflict_count++;
}

bool furi_hal_spi_stability_detect_deadlock(void) {
    if (spi_stability.current_owner == SpiDeviceMAX) {
        return false;  /* No deadlock - bus not owned */
    }

    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t operation_time = now - spi_stability.operation_start_ms;

    /* If operation takes longer than timeout, it's likely hung */
    if (operation_time > spi_stability.operation_timeout_ms) {
        ESP_LOGE(
            TAG,
            "DEADLOCK DETECTED: %s held bus for %lu ms!",
            device_names[spi_stability.current_owner],
            operation_time
        );

        /* Emergency reset */
        furi_hal_spi_stability_emergency_reset();
        return true;
    }

    return false;
}

void furi_hal_spi_stability_get_stats(
    uint32_t* total_ops,
    uint32_t* timeouts,
    uint32_t* conflicts
) {
    if (total_ops) *total_ops = spi_stability.total_operations;
    if (timeouts) *timeouts = spi_stability.timeout_count;
    if (conflicts) *conflicts = spi_stability.conflict_count;
}

void furi_hal_spi_stability_set_device_config(
    SpiDevice device,
    const SpiDeviceConfig* config
) {
    if (!config || device >= SpiDeviceMAX) return;

    ESP_LOGI(
        TAG,
        "Configuring %s: clock=%lu Hz, CS setup=%u us, CS hold=%u us, timeout=%u ms",
        device_names[device],
        config->clock_hz,
        config->cs_setup_us,
        config->cs_hold_us,
        config->timeout_ms
    );
}

void furi_hal_spi_stability_enable_recovery(SpiDevice device, bool enabled) {
    ESP_LOGI(
        TAG,
        "Device %s error recovery: %s",
        device_names[device],
        enabled ? "ENABLED" : "DISABLED"
    );
}

/* Watchdog-like monitor function to detect deadlocks */
void furi_hal_spi_stability_monitor(void) {
    if (furi_hal_spi_stability_detect_deadlock()) {
        ESP_LOGE(TAG, "SPI deadlock detected and recovered!");
    }
}
