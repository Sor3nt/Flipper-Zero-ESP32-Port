#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * USB Mass Storage Class (MSC) — exposes the SD card as a USB-Stick to the
 * host PC. Sits on top of the TinyUSB Composite Device (HID + CDC + MSC)
 * defined in furi_hal_usb_tinyusb_composite.c.
 *
 * The composite descriptor always includes an MSC interface (the descriptor
 * cannot be reconfigured at runtime). When MSC is *not* started, the SCSI
 * callbacks reply "no medium" and the host sees the drive as offline.
 *
 * Concurrency: the caller MUST ensure no firmware code is accessing the SD
 * while MSC is active. Typical flow:
 *   1) storage_sd_unmount() — close all files, unmount FATFS in storage svc
 *   2) furi_hal_sd_release_fatfs() — release FATFS in HAL, keep card alive
 *   3) furi_hal_usb_msc_start()
 *   ... host reads/writes the card ...
 *   4) furi_hal_usb_msc_stop()
 *   5) storage_sd_mount() — re-mount FATFS
 */

/** Activate the MSC layer. SD card must already be initialized.
 * @return true if MSC is now exposing the SD to the host.
 */
bool furi_hal_usb_msc_start(void);

/** Deactivate the MSC layer. Host will see the drive go offline.
 * After this returns, the SD card may be re-mounted by the firmware.
 */
void furi_hal_usb_msc_stop(void);

/** @return true if MSC is currently active. */
bool furi_hal_usb_msc_is_active(void);

#ifdef __cplusplus
}
#endif
