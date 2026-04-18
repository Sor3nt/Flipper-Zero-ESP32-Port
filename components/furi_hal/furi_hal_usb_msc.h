#pragma once

#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** True when firmware is built with TinyUSB MSC (ESP32-S3 / USB OTG builds). */
bool furi_hal_usb_msc_is_supported(void);

/** USB MSC session is active (SD is exposed to host, local FatFs is unmounted). */
bool furi_hal_usb_msc_is_active(void);

/**
 * Start USB mass-storage on the internal SD card.
 * Call storage_sd_suspend_for_usb_msc() first so FatFs releases the volume.
 */
esp_err_t furi_hal_usb_msc_start(void);

/** Tear down USB MSC so the SD card can be remounted locally. */
esp_err_t furi_hal_usb_msc_stop(void);

#ifdef __cplusplus
}
#endif
