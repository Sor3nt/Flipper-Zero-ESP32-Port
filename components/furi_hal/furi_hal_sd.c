/**
 * @file furi_hal_sd.c
 * SD Card HAL for ESP32-C6 (Waveshare ESP32-C6-LCD-1.9)
 *
 * Uses ESP-IDF SDSPI driver + raw FatFs diskio registration.
 * Shares SPI2_HOST with the display (CS-based device selection).
 */

#include "furi_hal_sd.h"
#include "../fatfs.h"
#include "furi_hal_resources.h"
#include "furi_hal_spi.h"
#include "furi_hal_spi_bus.h"

#include <inttypes.h>
#include <string.h>
#include <ff.h>
#include <diskio_impl.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <sdmmc_cmd.h>
#include <driver/sdspi_host.h>

static const char* TAG = "FuriHalSd";

#define SD_FATFS_DRIVE "0:"
#define SD_SPI_HOST    SPI2_HOST
#define SD_MAX_FREQ    (20 * 1000) /* 20 MHz — conservative for shared bus */

static sdmmc_card_t* sd_card = NULL;
static sdspi_dev_handle_t sd_handle = 0;
static bool sd_initialized = false;
static bool sd_diskio_registered = false;
static bool sd_disk_status_check = false;
static bool sd_mounted = false;
static const BYTE sd_pdrv = 0;

static bool sd_host_conflicts_with(const FuriHalSpiBus* bus) {
    if(!bus || !bus->initialized) return false;

    /* No conflict if bus shares the same SPI host (CS-based multiplexing) */
    if(bus->host_id == SD_SPI_HOST) return false;

    return bus->mosi_pin == gpio_sdcard_miso.pin || bus->miso_pin == gpio_sdcard_miso.pin ||
           bus->mosi_pin == gpio_sdcard_cs.pin || bus->miso_pin == gpio_sdcard_cs.pin ||
           bus->sck_pin == gpio_sdcard_cs.pin;
}

static uint16_t sd_read_le16(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t sd_read_le32(const uint8_t* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static bool sd_has_boot_signature(const uint8_t* sector) {
    return sector[510] == 0x55 && sector[511] == 0xAA;
}

static bool sd_sector_looks_like_mbr(const uint8_t* sector) {
    if(!sd_has_boot_signature(sector)) return false;

    for(size_t i = 0; i < 4; i++) {
        const uint8_t* entry = &sector[446 + i * 16];
        if(entry[4] != 0 || sd_read_le32(&entry[8]) != 0 || sd_read_le32(&entry[12]) != 0) {
            return true;
        }
    }

    return false;
}

static const char* sd_partition_type_name(uint8_t type) {
    switch(type) {
    case 0x00:
        return "empty";
    case 0x01:
        return "FAT12";
    case 0x04:
    case 0x06:
    case 0x0E:
        return "FAT16";
    case 0x0B:
    case 0x0C:
        return "FAT32";
    case 0x07:
        return "exFAT/NTFS";
    case 0x83:
        return "Linux";
    case 0xEE:
        return "GPT-protective";
    default:
        return "unknown";
    }
}

static const char* sd_detect_sector_format(const uint8_t* sector) {
    if(!sd_has_boot_signature(sector)) return "missing-boot-signature";
    if(memcmp(&sector[3], "EXFAT   ", 8) == 0) return "exFAT";
    if(memcmp(&sector[54], "FAT12   ", 8) == 0) return "FAT12";
    if(memcmp(&sector[54], "FAT16   ", 8) == 0) return "FAT16";
    if(memcmp(&sector[82], "FAT32   ", 8) == 0) return "FAT32";
    if(sd_sector_looks_like_mbr(sector)) return "MBR";
    return "unknown";
}

static void sd_make_hex_preview(const uint8_t* data, size_t len, char* out, size_t out_size) {
    if(out_size == 0) return;
    out[0] = '\0';

    size_t pos = 0;
    for(size_t i = 0; i < len && pos + 4 < out_size; i++) {
        int written = snprintf(&out[pos], out_size - pos, "%02X%s", data[i], (i + 1 < len) ? " " : "");
        if(written < 0) break;
        pos += (size_t)written;
    }
}

static void sd_log_boot_sector_details(const char* label, const uint8_t* sector) {
    const char* format = sd_detect_sector_format(sector);
    char preview[16 * 3] = {0};
    sd_make_hex_preview(sector, 16, preview, sizeof(preview));

    ESP_LOGI(
        TAG,
        "%s: detected=%s sig=%02X%02X first16=[%s]",
        label,
        format,
        sector[510],
        sector[511],
        preview);

    if(strcmp(format, "FAT12") == 0 || strcmp(format, "FAT16") == 0) {
        uint16_t bytes_per_sector = sd_read_le16(&sector[11]);
        uint8_t sectors_per_cluster = sector[13];
        uint16_t reserved = sd_read_le16(&sector[14]);
        uint8_t fats = sector[16];
        uint32_t total_sectors = sd_read_le16(&sector[19]);
        if(total_sectors == 0) total_sectors = sd_read_le32(&sector[32]);
        ESP_LOGI(
            TAG,
            "%s: FAT bytes_per_sector=%u sectors_per_cluster=%u reserved=%u fats=%u total_sectors=%" PRIu32,
            label,
            bytes_per_sector,
            sectors_per_cluster,
            reserved,
            fats,
            total_sectors);
    } else if(strcmp(format, "FAT32") == 0) {
        uint16_t bytes_per_sector = sd_read_le16(&sector[11]);
        uint8_t sectors_per_cluster = sector[13];
        uint16_t reserved = sd_read_le16(&sector[14]);
        uint8_t fats = sector[16];
        uint32_t fat_size = sd_read_le32(&sector[36]);
        uint32_t root_cluster = sd_read_le32(&sector[44]);
        uint32_t total_sectors = sd_read_le32(&sector[32]);
        ESP_LOGI(
            TAG,
            "%s: FAT32 bytes_per_sector=%u sectors_per_cluster=%u reserved=%u fats=%u fat_size=%" PRIu32 " root_cluster=%" PRIu32 " total_sectors=%" PRIu32,
            label,
            bytes_per_sector,
            sectors_per_cluster,
            reserved,
            fats,
            fat_size,
            root_cluster,
            total_sectors);
    } else if(strcmp(format, "exFAT") == 0) {
        uint32_t fat_offset = sd_read_le32(&sector[80]);
        uint32_t fat_length = sd_read_le32(&sector[84]);
        uint32_t cluster_heap_offset = sd_read_le32(&sector[88]);
        uint32_t cluster_count = sd_read_le32(&sector[92]);
        uint32_t root_dir_cluster = sd_read_le32(&sector[96]);
        uint32_t bytes_per_sector = 1u << sector[108];
        uint32_t sectors_per_cluster = 1u << sector[109];
        ESP_LOGW(
            TAG,
            "%s: exFAT detected bytes_per_sector=%" PRIu32 " sectors_per_cluster=%" PRIu32 " fat_offset=%" PRIu32 " fat_length=%" PRIu32 " cluster_heap=%" PRIu32 " cluster_count=%" PRIu32 " root_dir_cluster=%" PRIu32,
            label,
            bytes_per_sector,
            sectors_per_cluster,
            fat_offset,
            fat_length,
            cluster_heap_offset,
            cluster_count,
            root_dir_cluster);
    }
}

static void sd_log_mount_hint(const char* label, const uint8_t* sector) {
    const char* format = sd_detect_sector_format(sector);

    if(strcmp(format, "exFAT") == 0) {
        ESP_LOGW(
            TAG,
            "%s: unsupported filesystem exFAT. This firmware currently supports FAT12/16/32 only. Reformat the card or first partition as FAT32 (MBR).",
            label);
    } else if(strcmp(format, "missing-boot-signature") == 0) {
        ESP_LOGW(TAG, "%s: sector has no valid boot signature; no mountable FAT volume found", label);
    } else if(strcmp(format, "unknown") == 0) {
        ESP_LOGW(TAG, "%s: unknown boot sector layout; expected FAT12/16/32", label);
    }
}

static uint32_t sd_log_mbr_partitions(const uint8_t* sector) {
    uint32_t first_partition_lba = 0;

    for(size_t i = 0; i < 4; i++) {
        const uint8_t* entry = &sector[446 + i * 16];
        uint8_t type = entry[4];
        uint32_t start_lba = sd_read_le32(&entry[8]);
        uint32_t sector_count = sd_read_le32(&entry[12]);

        if(type == 0 && start_lba == 0 && sector_count == 0) continue;

        ESP_LOGI(
            TAG,
            "MBR partition %u: status=0x%02X type=0x%02X (%s) start_lba=%" PRIu32 " sectors=%" PRIu32,
            (unsigned)i,
            entry[0],
            type,
            sd_partition_type_name(type),
            start_lba,
            sector_count);

        if(first_partition_lba == 0 && start_lba != 0) {
            first_partition_lba = start_lba;
        }
    }

    return first_partition_lba;
}

static void sd_log_card_diagnostics(const sdmmc_card_t* card) {
    ESP_LOGI(
        TAG,
        "Diagnostic probe: mem=%" PRIu32 " sdio=%" PRIu32 " mmc=%" PRIu32 " ocr=0x%08" PRIX32 " sector_size=%d capacity_sectors=%d max_freq=%" PRIu32 " real_freq=%d",
        (uint32_t)card->is_mem,
        (uint32_t)card->is_sdio,
        (uint32_t)card->is_mmc,
        (uint32_t)card->ocr,
        card->csd.sector_size,
        card->csd.capacity,
        (uint32_t)card->max_freq_khz,
        card->real_freq_khz);
    sdmmc_card_print_info(stdout, card);
}

static void sd_probe_filesystem(const sdmmc_card_t* card) {
    uint8_t* sector = heap_caps_calloc(1, 512, MALLOC_CAP_DMA);
    if(!sector) {
        ESP_LOGW(TAG, "Diagnostic probe: failed to allocate sector buffer");
        return;
    }

    esp_err_t ret = sdmmc_read_sectors((sdmmc_card_t*)card, sector, 0, 1);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "Diagnostic probe: reading LBA0 failed: %s", esp_err_to_name(ret));
        free(sector);
        return;
    }

    sd_log_boot_sector_details("LBA0", sector);
    sd_log_mount_hint("LBA0", sector);

    if(sd_sector_looks_like_mbr(sector)) {
        uint32_t first_partition_lba = sd_log_mbr_partitions(sector);
        if(first_partition_lba != 0) {
            ret = sdmmc_read_sectors((sdmmc_card_t*)card, sector, first_partition_lba, 1);
            if(ret == ESP_OK) {
                sd_log_boot_sector_details("Partition0", sector);
                sd_log_mount_hint("Partition0", sector);
            } else {
                ESP_LOGW(
                    TAG,
                    "Diagnostic probe: reading partition boot sector at LBA %" PRIu32 " failed: %s",
                    first_partition_lba,
                    esp_err_to_name(ret));
            }
        }
    }

    free(sector);
}

static void sd_run_mount_diagnostics(const sdspi_device_config_t* dev_cfg, const sdmmc_host_t* host) {
    ESP_LOGW(TAG, "Running SD diagnostics after mount failure");

    esp_err_t ret = host->init ? host->init() : ESP_OK;
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "Diagnostic probe: host init failed: %s", esp_err_to_name(ret));
        return;
    }

    sdspi_dev_handle_t handle = 0;
    ret = sdspi_host_init_device(dev_cfg, &handle);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "Diagnostic probe: init device failed: %s", esp_err_to_name(ret));
        sdspi_host_deinit();
        return;
    }

    sdmmc_host_t probe_host = *host;
    probe_host.slot = handle;

    sdmmc_card_t card;
    memset(&card, 0, sizeof(card));

    ret = sdmmc_card_init(&probe_host, &card);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "Diagnostic probe: card_init failed: %s", esp_err_to_name(ret));
        sdspi_host_remove_device(handle);
        sdspi_host_deinit();
        return;
    }

    sd_log_card_diagnostics(&card);
    sd_probe_filesystem(&card);

    sdspi_host_remove_device(handle);
    sdspi_host_deinit();
}

static DSTATUS sd_fatfs_card_available(BYTE pdrv) {
    if((pdrv != sd_pdrv) || (sd_card == NULL)) {
        return STA_NOINIT;
    }

    furi_hal_spi_bus_lock();
    const esp_err_t err = sdmmc_get_status(sd_card);
    furi_hal_spi_bus_unlock();

    if(err != ESP_OK) {
        ESP_LOGE(TAG, "SD status check failed: %s", esp_err_to_name(err));
        return STA_NOINIT;
    }

    return 0;
}

static DSTATUS sd_fatfs_initialize(BYTE pdrv) {
    return sd_fatfs_card_available(pdrv);
}

static DSTATUS sd_fatfs_status(BYTE pdrv) {
    if(sd_disk_status_check) {
        return sd_fatfs_card_available(pdrv);
    }

    return ((pdrv == sd_pdrv) && (sd_card != NULL)) ? 0 : STA_NOINIT;
}

static DRESULT sd_fatfs_read(BYTE pdrv, BYTE* buff, DWORD sector, UINT count) {
    if((pdrv != sd_pdrv) || (sd_card == NULL)) {
        return RES_PARERR;
    }

    furi_hal_spi_bus_lock();
    const esp_err_t err = sdmmc_read_sectors(sd_card, buff, sector, count);
    furi_hal_spi_bus_unlock();

    if(err != ESP_OK) {
        ESP_LOGE(TAG, "SD read failed: %s", esp_err_to_name(err));
        return RES_ERROR;
    }

    return RES_OK;
}

static DRESULT sd_fatfs_write(BYTE pdrv, const BYTE* buff, DWORD sector, UINT count) {
    if((pdrv != sd_pdrv) || (sd_card == NULL)) {
        return RES_PARERR;
    }

    furi_hal_spi_bus_lock();
    const esp_err_t err = sdmmc_write_sectors(sd_card, buff, sector, count);
    furi_hal_spi_bus_unlock();

    if(err != ESP_OK) {
        ESP_LOGE(TAG, "SD write failed: %s", esp_err_to_name(err));
        return RES_ERROR;
    }

    return RES_OK;
}

static DRESULT sd_fatfs_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if((pdrv != sd_pdrv) || (sd_card == NULL)) {
        return RES_PARERR;
    }

    switch(cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *((DWORD*)buff) = sd_card->csd.capacity;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *((WORD*)buff) = sd_card->csd.sector_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        return RES_ERROR;
#if FF_USE_TRIM
    case CTRL_TRIM: {
        if(sdmmc_can_trim(sd_card) != ESP_OK) {
            return RES_PARERR;
        }

        const DWORD start_sector = ((DWORD*)buff)[0];
        const DWORD end_sector = ((DWORD*)buff)[1];

        furi_hal_spi_bus_lock();
        const esp_err_t err = sdmmc_erase_sectors(
            sd_card,
            start_sector,
            end_sector - start_sector + 1,
            sdmmc_can_discard(sd_card) == ESP_OK ? SDMMC_DISCARD_ARG : SDMMC_ERASE_ARG);
        furi_hal_spi_bus_unlock();

        return (err == ESP_OK) ? RES_OK : RES_ERROR;
    }
#endif
    default:
        return RES_ERROR;
    }
}

static void sd_register_fatfs_diskio(void) {
    static const ff_diskio_impl_t sd_fatfs_impl = {
        .init = &sd_fatfs_initialize,
        .status = &sd_fatfs_status,
        .read = &sd_fatfs_read,
        .write = &sd_fatfs_write,
        .ioctl = &sd_fatfs_ioctl,
    };

    ff_diskio_register(sd_pdrv, &sd_fatfs_impl);
    sd_disk_status_check = false;
    sd_diskio_registered = true;
}

static void sd_release_host(void) {
    if(sd_diskio_registered) {
        ff_diskio_unregister(sd_pdrv);
        sd_diskio_registered = false;
    }

    if(sd_handle) {
        sdspi_host_remove_device(sd_handle);
        sd_handle = 0;
    }

    sdspi_host_deinit();

    free(sd_card);
    sd_card = NULL;
    sd_initialized = false;
    sd_mounted = false;
    fatfs_init();
}

static bool sd_prepare_card(void) {
    if(sd_initialized && sd_card != NULL) {
        return true;
    }

    if(sd_host_conflicts_with(&furi_hal_spi_bus_external) ||
       sd_host_conflicts_with(&furi_hal_spi_bus_subghz)) {
        ESP_LOGW(
            TAG,
            "Skipping SD init because SPI2_HOST is reserved for the external CC1101 pin set");
        return false;
    }

    sd_release_host();

    ESP_LOGI(TAG, "Initializing SD card on SPI2_HOST, CS=GPIO%d", gpio_sdcard_cs.pin);

    sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev_cfg.host_id = SD_SPI_HOST;
    dev_cfg.gpio_cs = gpio_sdcard_cs.pin;
    dev_cfg.gpio_cd = SDSPI_SLOT_NO_CD;
    dev_cfg.gpio_wp = SDSPI_SLOT_NO_WP;
    dev_cfg.gpio_int = SDSPI_SLOT_NO_INT;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = SD_MAX_FREQ;

    ESP_LOGI(
        TAG,
        "SDSPI config: host=%d cs=%d cd=%d wp=%d int=%d target_freq_khz=%d",
        dev_cfg.host_id,
        dev_cfg.gpio_cs,
        dev_cfg.gpio_cd,
        dev_cfg.gpio_wp,
        dev_cfg.gpio_int,
        host.max_freq_khz);

    esp_err_t ret = ESP_OK;
    furi_hal_spi_bus_lock();
    do {
        ret = sdspi_host_init();
        if(ret != ESP_OK) break;

        ret = sdspi_host_init_device(&dev_cfg, &sd_handle);
        if(ret != ESP_OK) break;

        sd_card = calloc(1, sizeof(sdmmc_card_t));
        if(sd_card == NULL) {
            ret = ESP_ERR_NO_MEM;
            break;
        }

        host.slot = sd_handle;
        ret = sdmmc_card_init(&host, sd_card);
    } while(false);
    furi_hal_spi_bus_unlock();

    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "SD init failed: %s", esp_err_to_name(ret));
        sd_run_mount_diagnostics(&dev_cfg, &host);
        sd_release_host();
        return false;
    }

    fatfs_init();
    sd_register_fatfs_diskio();
    sd_initialized = true;

    sdmmc_card_print_info(stdout, sd_card);
    return true;
}

void furi_hal_sd_presence_init(void) {
    /* No hardware detect pin on this board */
    fatfs_init();
}

bool furi_hal_sd_is_present(void) {
    if(sd_host_conflicts_with(&furi_hal_spi_bus_external) ||
       sd_host_conflicts_with(&furi_hal_spi_bus_subghz)) {
        return false;
    }

    /* Without a detect pin, try init on demand and let the mount logic decide. */
    return true;
}

FuriStatus furi_hal_sd_init(bool power_reset) {
    (void)power_reset;
    return sd_prepare_card() ? FuriStatusOk : FuriStatusError;
}

FuriStatus furi_hal_sd_get_card_state(void) {
    return (sd_initialized && sd_card != NULL) ? FuriStatusOk : FuriStatusError;
}

FuriStatus furi_hal_sd_info(FuriHalSdInfo* info) {
    if(!info) return FuriStatusError;
    if(!sd_initialized || !sd_card) return FuriStatusError;

    memset(info, 0, sizeof(FuriHalSdInfo));

    info->manufacturer_id = (uint8_t)sd_card->cid.mfg_id;

    /* oem_id is an int in ESP-IDF; extract as 2 ASCII chars */
    info->oem_id[0] = (char)((sd_card->cid.oem_id >> 8) & 0xFF);
    info->oem_id[1] = (char)(sd_card->cid.oem_id & 0xFF);
    info->oem_id[2] = '\0';

    strncpy(info->product_name, sd_card->cid.name, sizeof(info->product_name) - 1);
    info->product_name[sizeof(info->product_name) - 1] = '\0';

    info->product_revision_major = (sd_card->cid.revision >> 4) & 0x0F;
    info->product_revision_minor = sd_card->cid.revision & 0x0F;
    info->product_serial_number = sd_card->cid.serial;
    info->manufacturing_month = sd_card->cid.date % 100;
    info->manufacturing_year = 2000 + sd_card->cid.date / 100;

    /* Capacity in bytes = sector count * sector size */
    info->sector_size = sd_card->csd.sector_size;
    info->capacity = (uint64_t)sd_card->csd.capacity * sd_card->csd.sector_size;

    return FuriStatusOk;
}

uint8_t furi_hal_sd_max_mount_retry_count(void) {
    return 3;
}

bool furi_hal_sd_mount(void) {
    if(sd_mounted) return true;
    if(!sd_prepare_card()) return false;

    const FRESULT result = f_mount(&fatfs_object, SD_FATFS_DRIVE, 1);
    if(result != FR_OK && result != FR_NO_FILESYSTEM) {
        ESP_LOGE(TAG, "FatFs mount failed: %d", result);
        return false;
    }

    sd_mounted = true;
    ESP_LOGI(TAG, "SD card mounted at " SD_FATFS_DRIVE);
    return true;
}

bool furi_hal_sd_unmount(void) {
    if(!sd_initialized) return true;

    const FRESULT result = f_mount(NULL, SD_FATFS_DRIVE, 0);
    if(result != FR_OK) {
        ESP_LOGE(TAG, "FatFs unmount failed: %d", result);
        return false;
    }

    ESP_LOGI(TAG, "SD card unmounted");
    sd_release_host();
    return true;
}

bool furi_hal_sd_is_mounted(void) {
    return sd_mounted;
}

bool furi_hal_sd_fatfs_detach_keep_card(void) {
    if(!sd_initialized || sd_card == NULL) {
        return false;
    }

    if(!sd_mounted) {
        return true;
    }

    const FRESULT result = f_mount(NULL, SD_FATFS_DRIVE, 0);
    if(result != FR_OK) {
        ESP_LOGE(TAG, "FatFs detach failed: %d", result);
        return false;
    }

    sd_mounted = false;
    ESP_LOGI(TAG, "FatFs detached (card kept)");
    return true;
}

bool furi_hal_sd_fatfs_remount_after_usb(void) {
    if(!sd_initialized || sd_card == NULL) {
        return false;
    }

    if(sd_mounted) {
        return true;
    }

    const FRESULT result = f_mount(&fatfs_object, SD_FATFS_DRIVE, 1);
    if(result != FR_OK && result != FR_NO_FILESYSTEM) {
        ESP_LOGE(TAG, "FatFs remount failed: %d", result);
        return false;
    }

    sd_mounted = true;
    ESP_LOGI(TAG, "FatFs remounted");
    return true;
}

struct sdmmc_card* furi_hal_sd_get_mmc_card(void) {
    return (struct sdmmc_card*)((sd_initialized && sd_card != NULL) ? sd_card : NULL);
}
