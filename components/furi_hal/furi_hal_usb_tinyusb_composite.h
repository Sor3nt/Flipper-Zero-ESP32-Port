#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Lazy one-shot installer for the TinyUSB Composite Device used on
 * ESP32-S3-class boards (HID + CDC-ACM with IAD).
 *
 * `esp_tinyusb` does not expose a reliable `uninstall`. Once installed, the
 * Composite stays up for the rest of the boot cycle. VID/PID/strings can only
 * be set on the *first* call; later calls return true but log a warning.
 *
 * Returns true if installed (either now or previously). Pass NULLs/zeros for
 * fields that should fall back to defaults.
 */
bool furi_hal_usb_composite_install(
    uint16_t vid,
    uint16_t pid,
    const char* manuf,
    const char* product);

bool furi_hal_usb_composite_is_installed(void);

/**
 * Tear down the Composite and route the internal USB FSLS PHY back to the
 * USB-Serial-JTAG controller, restoring the flash/console port without a
 * reboot. ESP32-S3/S2 only; returns false on boards without USB-OTG.
 *
 * NOTE: experimental. esp_tinyusb's tusb_teardown() is a no-op in this build,
 * so re-installing the Composite afterwards (without a reboot) is not
 * guaranteed to work. Intended for "leave qFlipper mode -> flash" flows.
 */
bool furi_hal_usb_composite_uninstall(void);

#ifdef __cplusplus
}
#endif
