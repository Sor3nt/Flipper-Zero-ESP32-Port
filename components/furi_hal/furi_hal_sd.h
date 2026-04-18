/**
 * @file furi_hal_sd.h
 * SD Card HAL for ESP32 (via ESP-IDF VFS)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <furi.h>

struct sdmmc_card;

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

/**
 * Unmount FatFs from the SD volume but keep the card powered and the SDSPI
 * host initialized (e.g. before handing the block device to USB MSC).
 */
bool furi_hal_sd_fatfs_detach_keep_card(void);

/** Remount FatFs after USB MSC releases the card. */
bool furi_hal_sd_fatfs_remount_after_usb(void);

/** Raw SD/MMC card handle for USB MSC (NULL if card is not initialized). */
struct sdmmc_card* furi_hal_sd_get_mmc_card(void);

#ifdef __cplusplus
}
#endif
