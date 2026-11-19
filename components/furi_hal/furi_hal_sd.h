/**
 * @file furi_hal_sd.h
 * SD Card HAL for ESP32 (via ESP-IDF VFS)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <furi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t manufacturer_id;
    char oem_id[3];
    char product_name[6];
    uint8_t product_revision_major;
    uint8_t product_revision_minor;
    uint32_t product_serial_number;
    uint8_t manufacturing_month;
    uint16_t manufacturing_year;
    uint64_t capacity;
    uint16_t sector_size;
} FuriHalSdInfo;

/** Initialize SD card presence detection */
void furi_hal_sd_presence_init(void);

/** Check if SD card is present */
bool furi_hal_sd_is_present(void);

/** Initialize SD card
 * @param power_reset Request reset before init. On the Plus SD recovery profile,
 * this detaches FatFs and reinitializes the owned driver/card at a bounded retry
 * speed; it does not toggle a power rail. The caller must exclude open files and
 * active USB MSC before requesting reset. Legacy profiles ignore this argument.
 * @return FuriStatusOk on success
 */
FuriStatus furi_hal_sd_init(bool power_reset);

/** Get SD card state
 * @return FuriStatusOk if card is ready
 */
FuriStatus furi_hal_sd_get_card_state(void);

/** Get SD card info
 * @param info  pointer to info struct to fill
 * @return FuriStatusOk on success
 */
FuriStatus furi_hal_sd_info(FuriHalSdInfo* info);

/** Get maximum mount retry count */
uint8_t furi_hal_sd_max_mount_retry_count(void);

/** Did the last init attempt find an empty slot?
 *
 * There is no card-detect pin, so furi_hal_sd_is_present() is always true.
 * This is the "card absent" signal instead: true only when the last
 * furi_hal_sd_init() got no answer to CMD0 at all. A slow or failing card
 * answers CMD0 and reports false, so it keeps its retries.
 *
 * @return true if nothing answered CMD0 on the last attempt
 */
bool furi_hal_sd_no_card_detected(void);

/** Mount SD card via ESP-IDF VFS at /sdcard
 * @return true on success
 */
bool furi_hal_sd_mount(void);

/** Unmount SD card
 * @return true on success
 */
bool furi_hal_sd_unmount(void);

/** Check if SD card is currently mounted */
bool furi_hal_sd_is_mounted(void);

/** Soft-unmount FATFS but keep the sdmmc card initialized.
 * Use this before handing the card to USB MSC. Re-mount via furi_hal_sd_mount().
 * @return true on success
 */
bool furi_hal_sd_release_fatfs(void);

/** Read raw sectors from the SD card with SPI bus lock held.
 * Safe to call from USB MSC SCSI read callbacks. FATFS must be released first
 * (or never mounted) to avoid concurrent access with the firmware FS.
 * @return true on success
 */
bool furi_hal_sd_read_sectors_raw(uint32_t sector, void* buf, uint32_t count);

/** Write raw sectors to the SD card with SPI bus lock held.
 * @return true on success
 */
bool furi_hal_sd_write_sectors_raw(uint32_t sector, const void* buf, uint32_t count);

/** Get total sector count of the active card, or 0 if not initialized. */
uint32_t furi_hal_sd_sector_count(void);

/** Get sector size of the active card in bytes, or 0 if not initialized. */
uint16_t furi_hal_sd_sector_size(void);

/* QFDIAG: diagnostic snapshot of the FatFs diskio layer. */
typedef struct {
    uint8_t inflight_op; /* 0 idle, 1 read, 2 write */
    uint32_t inflight_ms; /* how long the current op has been running */
    uint32_t lockwait_ms; /* how long it has been waiting for the SPI bus lock */
    uint32_t writes;
    uint32_t reads;
    uint32_t errors;
    uint32_t max_write_ms;
    uint32_t max_lockwait_ms;
    int32_t last_err;
    uint32_t write_sectors; /* cumulative */
    uint32_t write_busy_ms; /* cumulative time inside disk_write */
    uint32_t read_sectors; /* cumulative */
    uint32_t read_busy_ms; /* cumulative time inside disk_read */
} FuriHalSdDebug;
void furi_hal_sd_debug_get(FuriHalSdDebug* out);

#ifdef __cplusplus
}
#endif
