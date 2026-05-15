#include "furi_hal_usb_msc.h"

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2

#include <furi.h>
#include <string.h>

#include "furi_hal_sd.h"
#include "class/msc/msc.h"
#include "class/msc/msc_device.h"

#define TAG "FuriHalUsbMsc"

static volatile bool s_active = false;
/* Set to true while the host has locked the medium with SCSI Prevent/Allow
 * Medium Removal (=1). Indicates the host considers the drive mounted and
 * has unflushed cached writes; pulling the medium here corrupts the FS. */
static volatile bool s_removal_locked = false;
/* One-shot: tells the next TEST UNIT READY to fail with UNIT ATTENTION /
 * MEDIUM MAY HAVE CHANGED. This is the SCSI signal that makes a host that
 * has cached "no medium" re-run INQUIRY + READ_CAPACITY and pick up the
 * drive again. Crucial after an unsafe unplug, where macOS otherwise stops
 * polling and the volume never reappears until the next reboot. */
static volatile bool s_signal_medium_change = false;

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

    s_removal_locked = false;
    /* Arm the unit-attention shot before flipping active. The next host
     * TEST UNIT READY will fail with UNIT ATTENTION / 0x28 / 0x00, forcing
     * the host to re-probe the drive instead of trusting its cached "no
     * medium" state from a previous session. */
    s_signal_medium_change = true;
    s_active = true;
    FURI_LOG_I(
        TAG,
        "MSC started: %" PRIu32 " sectors x %u bytes (arming UNIT_ATTENTION)",
        furi_hal_sd_sector_count(),
        (unsigned)furi_hal_sd_sector_size());
    return true;
}

void furi_hal_usb_msc_stop(void) {
    if(!s_active) return;
    s_active = false;
    /* Tell the host the medium is gone. The next TEST UNIT READY answers
     * "not ready" with sense NOT_READY/MEDIUM_NOT_PRESENT, which is the
     * signal macOS/Linux/Windows interpret as an ejected USB drive. */
    tud_msc_set_sense(0, SCSI_SENSE_NOT_READY, 0x3A /*MEDIUM NOT PRESENT*/, 0x00);
    s_removal_locked = false;
    FURI_LOG_I(TAG, "MSC stopped (sense=NOT_READY/MEDIUM_NOT_PRESENT)");
}

bool furi_hal_usb_msc_is_active(void) {
    return s_active;
}

bool furi_hal_usb_msc_is_removal_locked(void) {
    return s_removal_locked;
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
        /* TinyUSB auto-fills sense = NOT_READY / MEDIUM_NOT_PRESENT (the
         * "stick was unplugged" signal) when we return false here and the
         * sense is otherwise clear. */
        return false;
    }
    if(s_signal_medium_change) {
        /* Once per start(): tell the host the medium just (re)appeared.
         * The host will follow up with REQUEST SENSE, read the UA, and
         * then re-INQUIRY + READ_CAPACITY — which is exactly what we want
         * because the host may have cached "no medium" from before. */
        s_signal_medium_change = false;
        tud_msc_set_sense(
            lun,
            SCSI_SENSE_UNIT_ATTENTION,
            0x28 /*NOT READY TO READY TRANSITION, MEDIUM MAY HAVE CHANGED*/,
            0x00);
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
        /* Host requested eject — clear the removal lock so the app can
         * leave cleanly without waiting for a second eject signal. We do
         * NOT call furi_hal_usb_msc_stop() here: that's the app's job once
         * the user presses Back. Otherwise re-mount would happen behind
         * the UI's back. */
        FURI_LOG_I(TAG, "Host ejected the medium");
        s_removal_locked = false;
    }
    return true;
}

bool tud_msc_prevent_allow_medium_removal_cb(
    uint8_t lun,
    uint8_t prohibit_removal,
    uint8_t control) {
    (void)lun;
    (void)control;
    /* The host sends PREVENT(=1) right after mount to "lock" the medium
     * (i.e. flag that there are unflushed buffers in its FS cache). It
     * sends ALLOW(=0) on eject. We use this as the signal of whether it's
     * safe to yank the SD back. */
    s_removal_locked = (prohibit_removal != 0);
    FURI_LOG_I(TAG, "Host %s removal", s_removal_locked ? "PREVENT" : "ALLOW");
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
