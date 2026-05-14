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
 * @param power_reset  perform power reset before init
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

#ifdef __cplusplus
}
#endif
