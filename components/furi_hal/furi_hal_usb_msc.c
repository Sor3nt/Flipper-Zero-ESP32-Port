#include "furi_hal_usb_msc.h"

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2

#include <furi.h>
#include <string.h>

#include "furi_hal_sd.h"
#include "class/msc/msc_device.h"

#define TAG "FuriHalUsbMsc"

static volatile bool s_active = false;

bool furi_hal_usb_msc_start(void) {
    if(s_active) return true;

    if(furi_hal_sd_get_card_state() != FuriStatusOk) {
        FURI_LOG_E(TAG, "SD card not initialized — refusing to start MSC");
        return false;
    }
    if(furi_hal_sd_is_mounted()) {
        FURI_LOG_E(TAG, "SD still mounted by firmware — release FATFS first");
        return false;
    }

    s_active = true;
    FURI_LOG_I(
        TAG,
        "MSC started: %" PRIu32 " sectors x %u bytes",
        furi_hal_sd_sector_count(),
        (unsigned)furi_hal_sd_sector_size());
    return true;
}

void furi_hal_usb_msc_stop(void) {
    if(!s_active) return;
    s_active = false;
    FURI_LOG_I(TAG, "MSC stopped");
}

bool furi_hal_usb_msc_is_active(void) {
    return s_active;
}

/* ─────────────────────────────────────────────────────────────────────
 * TinyUSB MSC SCSI callbacks
 *
 * Called from the TinyUSB task whenever the host issues a SCSI command on
 * the MSC interface. We back the LUN with the SD card through furi_hal_sd's
 * raw-sector API (which holds the SPI bus lock for us — important because
 * the SD shares SPI2_HOST with the display on the T-Embed).
 *
 * When s_active is false we reply with "no medium" so the host treats the
 * drive as offline (safe to eject) without disconnecting the whole device.
 * ───────────────────────────────────────────────────────────────────── */

static const char s_vendor_id[] = "Flipper ";
static const char s_product_id[] = "ESP32 SD-Card   ";
static const char s_product_rev[] = "1.0 ";

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id, s_vendor_id, 8);
    memcpy(product_id, s_product_id, 16);
    memcpy(product_rev, s_product_rev, 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    if(!s_active) {
        /* Per spec: when test_unit_ready returns false, follow up with a
         * "MEDIUM NOT PRESENT" sense via tud_msc_request_sense_cb. TinyUSB
         * pulls this automatically from tud_msc_get_sense_cb if available,
         * but the default path also works (sense data = 0). */
        return false;
    }
    return furi_hal_sd_get_card_state() == FuriStatusOk;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size) {
    (void)lun;
    if(!s_active) {
        *block_count = 0;
        *block_size = 512;
        return;
    }
    *block_count = furi_hal_sd_sector_count();
    *block_size = furi_hal_sd_sector_size();
    if(*block_size == 0) *block_size = 512;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun;
    (void)power_condition;
    (void)start;
    if(load_eject && !start) {
        /* Host requested eject — we honor it by stopping the MSC layer.
         * The firmware-side app will then re-mount FATFS on its own. */
        FURI_LOG_I(TAG, "Host requested eject");
        furi_hal_usb_msc_stop();
    }
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    (void)lun;
    if(!s_active) return -1;

    const uint16_t block_size = furi_hal_sd_sector_size();
    if(block_size == 0) return -1;

    /* TinyUSB calls us with (lba, offset, bufsize). offset is always 0 when
     * CFG_TUD_MSC_BUFSIZE is a multiple of block_size, which we ensure (4096). */
    if(offset != 0 || (bufsize % block_size) != 0) {
        FURI_LOG_E(
            TAG,
            "read10 misalignment: lba=%" PRIu32 " offset=%" PRIu32 " bufsize=%" PRIu32,
            lba,
            offset,
            bufsize);
        return -1;
    }

    const uint32_t count = bufsize / block_size;
    if(!furi_hal_sd_read_sectors_raw(lba, buffer, count)) {
        FURI_LOG_E(TAG, "read10 failed: lba=%" PRIu32 " count=%" PRIu32, lba, count);
        return -1;
    }
    return (int32_t)bufsize;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return s_active;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    (void)lun;
    if(!s_active) return -1;

    const uint16_t block_size = furi_hal_sd_sector_size();
    if(block_size == 0) return -1;

    if(offset != 0 || (bufsize % block_size) != 0) {
        FURI_LOG_E(
            TAG,
            "write10 misalignment: lba=%" PRIu32 " offset=%" PRIu32 " bufsize=%" PRIu32,
            lba,
            offset,
            bufsize);
        return -1;
    }

    const uint32_t count = bufsize / block_size;
    if(!furi_hal_sd_write_sectors_raw(lba, buffer, count)) {
        FURI_LOG_E(TAG, "write10 failed: lba=%" PRIu32 " count=%" PRIu32, lba, count);
        return -1;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize) {
    (void)lun;
    (void)scsi_cmd;
    (void)buffer;
    (void)bufsize;
    /* Reject all SCSI commands we don't implement explicitly above —
     * TinyUSB falls back to standard sense for these. */
    return -1;
}

#else /* !ESP32-S3 / S2 */

bool furi_hal_usb_msc_start(void) {
    return false;
}
void furi_hal_usb_msc_stop(void) {
}
bool furi_hal_usb_msc_is_active(void) {
    return false;
}

#endif
