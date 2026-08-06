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

/* Device configurations */
typedef struct {
    uint32_t clock_hz;
    uint8_t cs_hold_us;
    uint8_t cs_setup_us;
    uint16_t timeout_ms;
    bool recovery_enabled;
} DeviceConfigInternal;

static DeviceConfigInternal device_configs[SpiDeviceMAX] = {
    /* LCD */ {40000000, 5, 5, 100, true},
    /* SD */  {40000000, 5, 5, 5000, true},
    /* CC1101 */ {10000000, 3, 3, 500, true},
    /* NRF24 */ {10000000, 3, 3, 1000, true}
};

static volatile uint8_t spi_initialized = 0;

void furi_hal_spi_stability_init(void) {
    if (spi_initialized) {
        ESP_LOGW(TAG, "SPI Stability Manager already initialized");
        return;
    }

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
        vSemaphoreDelete(spi_stability.bus_mutex);
        return;
    }

    /* Initialize device status */
    for (int i = 0; i < SpiDeviceMAX; i++) {
        spi_stability.device_active[i] = 0;
        spi_stability.device_last_access_ms[i] = 0;
        spi_stability.device_priority[i] = device_base_priority[i];
    }

    spi_stability.current_owner = SpiDeviceMAX;  /* No owner initially */
    spi_stability.operation_timeout_ms = 5000;   /* 5 second default timeout */
    spi_initialized = 1;

    ESP_LOGI(TAG, "SPI Stability Manager initialized successfully");
}

bool furi_hal_spi_stability_acquire(
    SpiDevice device,
    SpiPriority priority,
    uint32_t timeout_ms
) {
    if (!spi_initialized) {
        ESP_LOGE(TAG, "SPI Stability Manager not initialized!");
        return false;
    }

    if (device >= SpiDeviceMAX) {
        ESP_LOGE(TAG, "Invalid device: %d", (int)device);
        return false;
    }

    if (!spi_stability.bus_mutex) {
        ESP_LOGE(TAG, "Bus mutex not created!");
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
            "Bus lock timeout for %s (timeout=%lu ms, owner=%s)",
            device_names[device],
            timeout_ms,
            spi_stability.current_owner < SpiDeviceMAX ? device_names[spi_stability.current_owner] : "NONE"
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

    /* Apply device-specific timeout if configured */
    if (device_configs[device].timeout_ms > 0) {
        spi_stability.operation_timeout_ms = device_configs[device].timeout_ms;
    }

    ESP_LOGV(TAG, "Bus acquired by %s (priority=%d)", device_names[device], priority);
    spi_stability.total_operations++;

    return true;
}

void furi_hal_spi_stability_release(SpiDevice device) {
    if (!spi_initialized) {
        ESP_LOGW(TAG, "SPI Manager not initialized on release");
        return;
    }

    if (device >= SpiDeviceMAX) {
        ESP_LOGW(TAG, "Invalid device on release: %d", device);
        return;
    }

    if (spi_stability.current_owner != device) {
        ESP_LOGW(
            TAG,
            "Release mismatch: %s attempting to release but owner is %s",
            device_names[device],
            spi_stability.current_owner < SpiDeviceMAX ? device_names[spi_stability.current_owner] : "NONE"
        );
        return;
    }

    spi_stability.current_owner = SpiDeviceMAX;
    spi_stability.device_active[device] = 0;

    /* Release mutex only once */
    if (spi_stability.bus_mutex) {
        xSemaphoreGive(spi_stability.bus_mutex);
    }

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
    if (!spi_initialized) return;

    ESP_LOGW(TAG, "EMERGENCY SPI BUS RESET! Owner was: %s",
        spi_stability.current_owner < SpiDeviceMAX ? device_names[spi_stability.current_owner] : "NONE");

    /* Safely release the bus lock - give it only once */
    if (spi_stability.bus_mutex) {
        BaseType_t result = xSemaphoreGive(spi_stability.bus_mutex);
        if (result != pdTRUE) {
            ESP_LOGW(TAG, "Failed to give bus mutex during emergency reset");
        }
    }

    /* Reset state */
    spi_stability.current_owner = SpiDeviceMAX;
    for (int i = 0; i < SpiDeviceMAX; i++) {
        spi_stability.device_active[i] = 0;
    }

    spi_stability.conflict_count++;
    ESP_LOGI(TAG, "SPI Bus emergency reset complete (conflicts: %lu)", spi_stability.conflict_count);
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
    if (!config || device >= SpiDeviceMAX) {
        ESP_LOGW(TAG, "Invalid device config parameters");
        return;
    }

    /* Store device configuration */
    device_configs[device].clock_hz = config->clock_hz;
    device_configs[device].cs_hold_us = config->cs_hold_us;
    device_configs[device].cs_setup_us = config->cs_setup_us;
    device_configs[device].timeout_ms = config->timeout_ms;
    device_configs[device].recovery_enabled = true;

    ESP_LOGI(
        TAG,
        "Device %s configured: clock=%lu Hz, CS setup=%u us, CS hold=%u us, timeout=%u ms",
        device_names[device],
        config->clock_hz,
        config->cs_setup_us,
        config->cs_hold_us,
        config->timeout_ms
    );
}

void furi_hal_spi_stability_enable_recovery(SpiDevice device, bool enabled) {
    if (device >= SpiDeviceMAX) {
        ESP_LOGW(TAG, "Invalid device for recovery config: %d", device);
        return;
    }

    device_configs[device].recovery_enabled = enabled;
    ESP_LOGI(
        TAG,
        "Device %s error recovery: %s",
        device_names[device],
        enabled ? "ENABLED" : "DISABLED"
    );
}

/* Watchdog-like monitor function to detect deadlocks */
void furi_hal_spi_stability_monitor(void) {
    if (!spi_initialized) return;

    if (furi_hal_spi_stability_detect_deadlock()) {
        ESP_LOGE(TAG, "SPI deadlock detected and recovered!");
    }
}

/* Get device configuration */
bool furi_hal_spi_stability_get_device_config(
    SpiDevice device,
    SpiDeviceConfig* config
) {
    if (!config || device >= SpiDeviceMAX) {
        return false;
    }

    config->clock_hz = device_configs[device].clock_hz;
    config->cs_hold_us = device_configs[device].cs_hold_us;
    config->cs_setup_us = device_configs[device].cs_setup_us;
    config->timeout_ms = device_configs[device].timeout_ms;
    config->priority = device_base_priority[device];

    return true;
}

/* Check if device has recovery enabled */
bool furi_hal_spi_stability_is_recovery_enabled(SpiDevice device) {
    if (device >= SpiDeviceMAX) return false;
    return device_configs[device].recovery_enabled;
}
