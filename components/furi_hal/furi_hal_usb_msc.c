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

/* When set, MSC serves an app-provided backing (a disk image / ISO / RAM disk)
 * instead of the raw SD card. The SD stays mounted; the backing callbacks run
 * on the TinyUSB task and reach the image through the firmware filesystem. */
static FuriHalUsbMscBacking s_backing;
static volatile bool s_has_backing = false;

bool furi_hal_usb_msc_start_backing(const FuriHalUsbMscBacking* backing) {
    if(s_active) {
        FURI_LOG_E(TAG, "MSC already active — refusing backing start");
        return false;
    }
    if(!backing || !backing->read || backing->block_count == 0) {
        FURI_LOG_E(TAG, "Invalid MSC backing");
        return false;
    }
    uint16_t bs = (backing->block_size != 0) ? (uint16_t)backing->block_size : 512;
    if(bs != 512 && bs != 2048) {
        FURI_LOG_E(TAG, "Unsupported MSC backing block size %u", (unsigned)bs);
        return false;
    }

    s_backing = *backing;
    s_backing.block_size = bs;
    /* A writable medium with no write callback would advertise read/write to the
     * host yet fail every WRITE10. Degrade to read-only instead. */
    if(s_backing.writable && !s_backing.write) {
        FURI_LOG_W(TAG, "MSC backing writable but no write callback — forcing read-only");
        s_backing.writable = false;
    }
    s_has_backing = true;
    s_removal_locked = false;
    /* Same UNIT_ATTENTION hand-off as the SD path: force the host to re-probe. */
    s_signal_medium_change = true;
    s_active = true;
    FURI_LOG_I(
        TAG,
        "MSC started (file-backed): %" PRIu32 " blocks x %u bytes, %s",
        backing->block_count,
        (unsigned)bs,
        backing->writable ? "rw" : "ro");
    return true;
}

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
    s_has_backing = false;
    /* The host now sees the medium as gone: with s_active false the next TEST
     * UNIT READY / READ / WRITE fails and TinyUSB fills the default sense
     * NOT_READY/MEDIUM_NOT_PRESENT itself (msc_device.c), which hosts treat as
     * an ejected drive. The sense is deliberately not written from here:
     * tud_msc_set_sense() is plain state of the TinyUSB task and this runs on
     * the app task, so a write here could interleave with REQUEST SENSE. */
    s_removal_locked = false;
    FURI_LOG_I(TAG, "MSC stopped (host sees MEDIUM_NOT_PRESENT)");
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
    if(s_has_backing) {
        memcpy(vendor_id, s_backing.vendor_id, 8);
        memcpy(product_id, s_backing.product_id, 16);
        memcpy(product_rev, s_backing.product_rev, 4);
        return;
    }
    memcpy(vendor_id, s_vendor_id, 8);
    memcpy(product_id, s_product_id, 16);
    memcpy(product_rev, s_product_rev, 4);
}

/* INQUIRY v2 hook: lets us set the peripheral device type. TinyUSB's built-in
 * INQUIRY hardcodes 0x00 (direct-access disk); a CD-ROM must report 0x05. For
 * the disk/SD paths we return 0 so TinyUSB falls back to the v1 string callback
 * above (device type stays 0x00). */
uint32_t tud_msc_inquiry2_cb(uint8_t lun, scsi_inquiry_resp_t* resp, uint32_t bufsize) {
    (void)lun;
    (void)bufsize;
    if(!(s_has_backing && s_backing.is_cdrom)) return 0; // not a CD-ROM → v1 path
    resp->peripheral_device_type = 0x05; // CD/DVD-ROM
    resp->peripheral_qualifier = 0;
    resp->is_removable = 1;
    resp->version = 0; // no version claimed, as most ATAPI drives report
    resp->response_data_format = 2;
    resp->additional_length = sizeof(scsi_inquiry_resp_t) - 5; // 31
    memcpy(resp->vendor_id, s_backing.vendor_id, 8);
    memcpy(resp->product_id, s_backing.product_id, 16);
    memcpy(resp->product_rev, s_backing.product_rev, 4);
    return sizeof(scsi_inquiry_resp_t); // 36
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
    /* A file-backed image is always "present" while active; only the raw-SD
     * path has to re-check that a physical card is still there. */
    if(s_has_backing) return true;
    return furi_hal_sd_get_card_state() == FuriStatusOk;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size) {
    (void)lun;
    if(!s_active) {
        *block_count = 0;
        *block_size = 512;
        return;
    }
    if(s_has_backing) {
        *block_count = s_backing.block_count;
        *block_size = (uint16_t)s_backing.block_size;
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
        /* Let a file-backed app react (e.g. close the running screen). */
        if(s_has_backing && s_backing.eject) s_backing.eject(s_backing.ctx);
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

    const uint16_t block_size = s_has_backing ? (uint16_t)s_backing.block_size :
                                                furi_hal_sd_sector_size();
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
    if(s_has_backing) {
        if(!s_backing.read(s_backing.ctx, lba, count, buffer)) {
            FURI_LOG_E(TAG, "backing read failed: lba=%" PRIu32 " count=%" PRIu32, lba, count);
            return -1;
        }
    } else if(!furi_hal_sd_read_sectors_raw(lba, buffer, count)) {
        FURI_LOG_E(TAG, "read10 failed: lba=%" PRIu32 " count=%" PRIu32, lba, count);
        return -1;
    }
    return (int32_t)bufsize;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    if(s_has_backing) return s_active && s_backing.writable;
    return s_active;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    (void)lun;
    if(!s_active) return -1;
    if(s_has_backing && !s_backing.writable) return -1;

    const uint16_t block_size = s_has_backing ? (uint16_t)s_backing.block_size :
                                                furi_hal_sd_sector_size();
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
    if(s_has_backing) {
        if(!s_backing.write || !s_backing.write(s_backing.ctx, lba, count, buffer)) {
            FURI_LOG_E(TAG, "backing write failed: lba=%" PRIu32 " count=%" PRIu32, lba, count);
            return -1;
        }
    } else if(!furi_hal_sd_write_sectors_raw(lba, buffer, count)) {
        FURI_LOG_E(TAG, "write10 failed: lba=%" PRIu32 " count=%" PRIu32, lba, count);
        return -1;
    }
    return (int32_t)bufsize;
}

/* ─────────────────────────────────────────────────────────────────────
 * CD-ROM (MMC) emulation
 *
 * When the active backing is optical (is_cdrom), the device must look like a
 * CD/DVD drive: INQUIRY type 0x05 (set in tud_msc_inquiry2_cb) plus the MMC
 * command set below. TinyUSB handles the SBC commands natively (TEST UNIT
 * READY, READ CAPACITY 10 → 2048-byte blocks via tud_msc_capacity_cb, READ10
 * via the backing), and routes everything else here through tud_msc_scsi_cb.
 * Response byte layouts follow MMC-5 and mirror the flippercd app (GPL-3.0).
 * ───────────────────────────────────────────────────────────────────── */

/* ≈703 MB. An ISO larger than this is advertised as a DVD-ROM profile. */
#define CDROM_CD_MAX_BLOCKS 360000u

static void cd_put_be16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void cd_put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static uint16_t cd_be16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
/* MSF address of an LBA with the +150-frame (2 s) pre-gap, layout [00][M][S][F]. */
static void cd_put_msf(uint8_t* p, uint32_t lba) {
    uint32_t f = lba + 150;
    p[0] = 0;
    p[1] = (uint8_t)(f / (75u * 60u));
    p[2] = (uint8_t)((f / 75u) % 60u);
    p[3] = (uint8_t)(f % 75u);
}
/* Clamp a built response to the host allocation length and the EP buffer. */
static int32_t cd_done(size_t built, uint32_t alloc, uint16_t bufsize) {
    uint32_t n = (uint32_t)built;
    if(n > alloc) n = alloc;
    if(n > bufsize) n = bufsize;
    return (int32_t)n;
}
static int32_t cd_invalid_field(uint8_t lun) {
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24 /*INVALID FIELD IN CDB*/, 0x00);
    return -1;
}

/* Append one GET CONFIGURATION feature if it passes the request-type / start
 * filter. Returns the new write index. code = feature code, ver = version,
 * cur = whether the CURRENT bit is set, data/len = feature-specific payload. */
static size_t cd_cfg_feature(
    uint8_t* buf,
    size_t idx,
    uint16_t code,
    uint8_t ver,
    bool cur,
    uint8_t rt,
    uint16_t start,
    const uint8_t* data,
    uint8_t len) {
    if(code < start) return idx; // below the starting feature → skip
    if(rt == 0x02 && code != start) return idx; // "one feature" request
    if(rt == 0x01 && !cur) return idx; // "current only" request
    cd_put_be16(&buf[idx], code);
    buf[idx + 2] = (uint8_t)((ver << 2) | 0x02 /*Persistent*/ | (cur ? 0x01 : 0x00));
    buf[idx + 3] = len;
    if(len && data) memcpy(&buf[idx + 4], data, len);
    return idx + 4u + len;
}

static int32_t
    cdrom_scsi_cmd(uint8_t lun, const uint8_t cmd[16], uint8_t* buf, uint16_t bufsize) {
    const uint32_t N = s_backing.block_count; // total 2048-byte blocks
    const bool dvd = N > CDROM_CD_MAX_BLOCKS;
    const uint16_t profile = dvd ? 0x0010 : 0x0008;

    switch(cmd[0]) {
    case 0x43: { // READ TOC/PMA/ATIP
        const bool msf = (cmd[1] & 0x02) != 0;
        uint8_t format = cmd[2] & 0x0F;
        if(format == 0) format = (uint8_t)(cmd[9] >> 6);
        const uint8_t track = cmd[6];
        const uint32_t alloc = cd_be16(&cmd[7]);
        size_t idx = 4;
        if(format == 0) {
            if(track > 1 && track != 0xAA) return cd_invalid_field(lun);
            if(track <= 1) {
                buf[idx++] = 0x00;
                buf[idx++] = 0x14; // ADR=1, Control=4 (data track)
                buf[idx++] = 0x01; // track number 1
                buf[idx++] = 0x00;
                if(msf) {
                    buf[idx++] = 0; buf[idx++] = 0; buf[idx++] = 2; buf[idx++] = 0;
                } else {
                    cd_put_be32(&buf[idx], 0); idx += 4;
                }
            }
            buf[idx++] = 0x00;
            buf[idx++] = 0x14;
            buf[idx++] = 0xAA; // lead-out
            buf[idx++] = 0x00;
            if(msf) {
                cd_put_msf(&buf[idx], N); idx += 4;
            } else {
                cd_put_be32(&buf[idx], N); idx += 4;
            }
            cd_put_be16(buf, (uint16_t)(idx - 2));
            buf[2] = 1; // first track
            buf[3] = 1; // last track
            return cd_done(idx, alloc, bufsize);
        } else if(format == 1) { // session info: one session, first data track
            buf[idx++] = 0x00; buf[idx++] = 0x14; buf[idx++] = 0x01; buf[idx++] = 0x00;
            if(msf) {
                buf[idx++] = 0; buf[idx++] = 0; buf[idx++] = 2; buf[idx++] = 0;
            } else {
                cd_put_be32(&buf[idx], 0); idx += 4;
            }
            cd_put_be16(buf, (uint16_t)(idx - 2));
            buf[2] = 1; // first session
            buf[3] = 1; // last session
            return cd_done(idx, alloc, bufsize);
        }
        return cd_invalid_field(lun);
    }

    case 0x46: { // GET CONFIGURATION
        const uint8_t rt = cmd[1] & 0x03;
        const uint16_t start = cd_be16(&cmd[2]);
        const uint32_t alloc = cd_be16(&cmd[7]);
        size_t idx = 8; // 8-byte header, filled last

        uint8_t d[8];
        // 0x0000 Profile List
        cd_put_be16(&d[0], 0x0010); d[2] = dvd ? 0x01 : 0x00; d[3] = 0;
        cd_put_be16(&d[4], 0x0008); d[6] = (N && !dvd) ? 0x01 : 0x00; d[7] = 0;
        idx = cd_cfg_feature(buf, idx, 0x0000, 0, true, rt, start, d, 8);
        // 0x0001 Core (physical interface = USB)
        cd_put_be32(&d[0], 0x00000008); d[4] = 0x01; d[5] = 0; d[6] = 0; d[7] = 0;
        idx = cd_cfg_feature(buf, idx, 0x0001, 1, true, rt, start, d, 8);
        // 0x0003 Removable Medium (tray loader, eject, lock)
        d[0] = 0x2D; d[1] = 0; d[2] = 0; d[3] = 0;
        idx = cd_cfg_feature(buf, idx, 0x0003, 0, true, rt, start, d, 4);
        // 0x0010 Random Readable (block size 2048)
        cd_put_be32(&d[0], 2048); cd_put_be16(&d[4], dvd ? 16 : 1); d[6] = 0; d[7] = 0;
        idx = cd_cfg_feature(buf, idx, 0x0010, 0, N != 0, rt, start, d, 8);

        cd_put_be32(buf, (uint32_t)(idx - 4)); // data length
        buf[4] = 0; buf[5] = 0;
        cd_put_be16(&buf[6], (N == 0) ? 0x0000 : profile); // current profile
        return cd_done(idx, alloc, bufsize);
    }

    case 0x4A: { // GET EVENT STATUS NOTIFICATION (polled only)
        if(!(cmd[1] & 0x01)) return cd_invalid_field(lun);
        const uint32_t alloc = cd_be16(&cmd[7]);
        const uint8_t req_classes = cmd[4];
        if(!(req_classes & 0x10)) { // media class (bit 4) not requested
            cd_put_be16(buf, 2); buf[2] = 0x80; /*NEA, no event*/ buf[3] = 0x10;
            return cd_done(4, alloc, bufsize);
        }
        cd_put_be16(buf, 6);
        buf[2] = 0x04; // event class = media
        buf[3] = 0x10; // supported event classes
        buf[4] = 0x00; // media event = no change
        buf[5] = (uint8_t)(N ? 0x02 : 0x01); // media present / tray open
        buf[6] = 0; buf[7] = 0;
        return cd_done(8, alloc, bufsize);
    }

    case 0x51: { // READ DISC INFORMATION (standard)
        if((cmd[1] & 0x07) != 0) return cd_invalid_field(lun);
        const uint32_t alloc = cd_be16(&cmd[7]);
        memset(buf, 0, 34);
        cd_put_be16(buf, 32);
        buf[2] = 0x0E; // last session complete, disc complete, not erasable
        buf[3] = 1; buf[4] = 1; buf[5] = 1; buf[6] = 1;
        buf[7] = 0x20; // unrestricted use
        cd_put_be32(&buf[16], 0xFFFFFFFF);
        cd_put_be32(&buf[20], 0xFFFFFFFF);
        return cd_done(34, alloc, bufsize);
    }

    case 0x52: { // READ TRACK INFORMATION
        const uint8_t type = cmd[1] & 0x03;
        const uint32_t ref = cd_be16(&cmd[4]); // track/session number (or low bits of LBA)
        if((type == 1 || type == 2) && ref > 1) return cd_invalid_field(lun); // only track/session 1
        const uint32_t alloc = cd_be16(&cmd[7]);
        memset(buf, 0, 36);
        cd_put_be16(buf, 34);
        buf[2] = 1; // track number
        buf[3] = 1; // session number
        buf[5] = 0x04; // track mode: data, uninterrupted
        buf[6] = 0x01; // data mode 1
        cd_put_be32(&buf[24], N); // track size
        cd_put_be32(&buf[28], (N ? N - 1 : 0)); // last recorded address
        return cd_done(36, alloc, bufsize);
    }

    case 0x5A: { // MODE SENSE (10)
        const uint8_t pc = (uint8_t)(cmd[2] >> 6);
        const uint8_t page = (uint8_t)(cmd[2] & 0x3F);
        const uint32_t alloc = cd_be16(&cmd[7]);
        if(pc == 3) {
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x39, 0x00); // saving not supported
            return -1;
        }
        if(page != 0x01 && page != 0x2A && page != 0x3F) return cd_invalid_field(lun);
        size_t idx = 8; // 8-byte mode parameter header
        if(page == 0x01 || page == 0x3F) { // Read Error Recovery
            memset(&buf[idx], 0, 12);
            buf[idx] = 0x01; buf[idx + 1] = 0x0A;
            idx += 12;
        }
        if((page == 0x2A || page == 0x3F) && pc != 1) { // CD Capabilities & Mechanical Status
            memset(&buf[idx], 0, 22);
            buf[idx] = 0x2A; buf[idx + 1] = 0x14;
            buf[idx + 2] = 0x08; // reads CD-R? no; DVD-ROM read if DVD
            buf[idx + 6] = 0x2D; // tray loader, eject, lock
            cd_put_be16(&buf[idx + 8], 1385); // max read speed kB/s
            cd_put_be16(&buf[idx + 12], 16); // buffer size KB
            cd_put_be16(&buf[idx + 14], 1385); // current read speed
            idx += 22;
        }
        memset(buf, 0, 8);
        cd_put_be16(buf, (uint16_t)(idx - 2)); // mode data length
        return cd_done(idx, alloc, bufsize);
    }

    case 0xBD: { // MECHANISM STATUS
        const uint32_t alloc = cd_be16(&cmd[8]);
        memset(buf, 0, 8);
        buf[1] = (uint8_t)(N ? 0x00 : 0x10); // door closed / open
        return cd_done(8, alloc, bufsize);
    }

    // Commands that only need an OK with no data.
    case 0x2B: // SEEK(10)
    case 0x2F: // VERIFY(10)
    case 0x35: // SYNCHRONIZE CACHE
    case 0xBB: // SET CD SPEED
        return 0;

    default:
        tud_msc_set_sense(
            lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20 /*INVALID COMMAND OPERATION CODE*/, 0x00);
        return -1;
    }
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize) {
    if(s_has_backing && s_backing.is_cdrom) {
        return cdrom_scsi_cmd(lun, scsi_cmd, (uint8_t*)buffer, bufsize);
    }
    /* Reject all SCSI commands we don't implement — TinyUSB falls back to
     * standard sense for these. */
    (void)lun;
    (void)scsi_cmd;
    (void)buffer;
    (void)bufsize;
    return -1;
}

#else /* !ESP32-S3 / S2 */

bool furi_hal_usb_msc_start(void) {
    return false;
}
bool furi_hal_usb_msc_start_backing(const FuriHalUsbMscBacking* backing) {
    (void)backing;
    return false;
}
void furi_hal_usb_msc_stop(void) {
}
bool furi_hal_usb_msc_is_active(void) {
    return false;
}
bool furi_hal_usb_msc_is_removal_locked(void) {
    return false;
}

#endif
