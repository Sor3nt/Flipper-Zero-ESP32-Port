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

/**
 * File/image-backed MSC.
 *
 * Instead of exposing the raw SD card, serve blocks from an app-provided
 * backing — a disk image file on the SD, a CD-ROM ISO, a RAM disk, ...
 *
 * Unlike furi_hal_usb_msc_start(), this does NOT unmount the SD and does NOT
 * require the SD to be released: the backing callbacks run on the TinyUSB task
 * and typically read the image *through* the mounted filesystem. The caller
 * owns the image file's lifetime (it must outlive the MSC session) and must
 * ensure no other task writes the image concurrently.
 *
 * The backing struct is copied, so a stack variable may be passed. ctx and the
 * function pointers must stay valid until furi_hal_usb_msc_stop().
 *
 * Only one of furi_hal_usb_msc_start() / _start_backing() can be active.
 */
typedef struct {
    void* ctx;
    uint32_t block_size; /* bytes per LBA: 512 for disk/floppy, 2048 for CD-ROM */
    uint32_t block_count; /* total number of addressable LBAs */
    bool writable; /* false => write-protected / read-only medium */
    bool is_cdrom; /* reserved: CD-ROM emulation (peripheral type 0x05 + MMC) */
    char vendor_id[8]; /* SCSI INQUIRY vendor  (space-padded, not NUL-terminated) */
    char product_id[16]; /* SCSI INQUIRY product (space-padded) */
    char product_rev[4]; /* SCSI INQUIRY revision (space-padded) */
    /* Read `count` blocks of block_size bytes starting at `lba` into `out`.
     * Runs on the TinyUSB task. Return true on success, false => MEDIUM ERROR. */
    bool (*read)(void* ctx, uint32_t lba, uint32_t count, uint8_t* out);
    /* Write `count` blocks starting at `lba`. Not called when !writable; may be
     * NULL in that case. Return true on success. */
    bool (*write)(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf);
    /* Host asked to eject (SCSI START STOP UNIT, load_eject). Optional (NULL). */
    void (*eject)(void* ctx);
} FuriHalUsbMscBacking;

/** Activate the MSC layer backed by `backing` (not the raw SD).
 * @return true if MSC is now exposing the backing to the host.
 */
bool furi_hal_usb_msc_start_backing(const FuriHalUsbMscBacking* backing);

/** Deactivate the MSC layer. Host will see the drive go offline.
 * After this returns, the SD card may be re-mounted by the firmware.
 */
void furi_hal_usb_msc_stop(void);

/** @return true if MSC is currently active. */
bool furi_hal_usb_msc_is_active(void);

/** @return true if the host has sent SCSI Prevent Medium Removal — i.e. the
 * host considers the volume mounted and probably has dirty cache for it.
 * Apps should warn the user to eject from the host before stopping MSC. */
bool furi_hal_usb_msc_is_removal_locked(void);

#ifdef __cplusplus
}
#endif
