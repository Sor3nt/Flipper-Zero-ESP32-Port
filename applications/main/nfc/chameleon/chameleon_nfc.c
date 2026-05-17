#include "chameleon_nfc.h"
#include "chameleon.h"

#include <nfc/protocols/mf_classic/mf_classic.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a.h>
#include <furi.h>
#include <esp_log.h>
#include <string.h>

#define TAG "ChameleonNfc"

static MfClassicType chameleon_nfc_mfc_type(uint8_t sak) {
    switch(sak & 0x7F) {
    case 0x09:
        return MfClassicTypeMini;
    case 0x18:
        return MfClassicType4k;
    default:
        return MfClassicType1k; /* 0x08 etc. — covers S50 / Gen3 */
    }
}

static bool chameleon_nfc_read_mf_classic(
    NfcDevice* device,
    const uint8_t* uid,
    uint8_t uid_len,
    const uint8_t atqa[2],
    uint8_t sak,
    volatile bool* abort_flag) {
    MfClassicType type = chameleon_nfc_mfc_type(sak);
    MfClassicData* mfc = mf_classic_alloc();
    mfc->type = type;

    Iso14443_3aData* iso = mf_classic_get_base_data(mfc);
    iso14443_3a_set_uid(iso, uid, uid_len);
    iso14443_3a_set_atqa(iso, atqa);
    iso14443_3a_set_sak(iso, sak);

    /* Well-known key dictionary. NDEF-formatted MIFARE Classic (e.g. written
     * by a phone) uses the MAD key on sector 0 and the NFC-Forum NDEF key on
     * the data sectors — NOT the factory default — so a default-only read
     * misses every NDEF sector and the clone comes out empty. */
    static const uint8_t KEYS[][6] = {
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, /* factory default */
        {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}, /* MAD key A (sector 0) */
        {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7}, /* NFC-Forum NDEF key A */
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5},
    };
    const size_t KEY_NUM = sizeof(KEYS) / sizeof(KEYS[0]);

    uint16_t total = mf_classic_get_total_block_num(type);
    uint8_t sectors = mf_classic_get_total_sectors_num(type);
    uint16_t read_ok = 0;
    uint16_t keys_found = 0;

    /* Per sector: find Key A and Key B by trying the dictionary on the
     * sector trailer, then store the *actual* key found (so the trailer the
     * dump/emulation presents carries the keys a reader expects). */
    const uint8_t* sec_key[MF_CLASSIC_TOTAL_SECTORS_MAX] = {0};
    uint8_t sec_kt[MF_CLASSIC_TOTAL_SECTORS_MAX] = {0};

    for(uint8_t s = 0; s < sectors; s++) {
        if(abort_flag && *abort_flag) break;
        uint8_t tb = mf_classic_get_sector_trailer_num_by_sector(s);
        uint8_t tmp[16];
        for(size_t k = 0; k < KEY_NUM; k++) {
            if(chameleon_mf1_read_block(tb, CHAMELEON_MF_KEY_A, KEYS[k], tmp)) {
                uint64_t kv = ((uint64_t)KEYS[k][0] << 40) | ((uint64_t)KEYS[k][1] << 32) |
                              ((uint64_t)KEYS[k][2] << 24) | ((uint64_t)KEYS[k][3] << 16) |
                              ((uint64_t)KEYS[k][4] << 8) | KEYS[k][5];
                mf_classic_set_key_found(mfc, s, MfClassicKeyTypeA, kv);
                sec_key[s] = KEYS[k];
                sec_kt[s] = CHAMELEON_MF_KEY_A;
                keys_found++;
                break;
            }
        }
        for(size_t k = 0; k < KEY_NUM; k++) {
            if(chameleon_mf1_read_block(tb, CHAMELEON_MF_KEY_B, KEYS[k], tmp)) {
                uint64_t kv = ((uint64_t)KEYS[k][0] << 40) | ((uint64_t)KEYS[k][1] << 32) |
                              ((uint64_t)KEYS[k][2] << 24) | ((uint64_t)KEYS[k][3] << 16) |
                              ((uint64_t)KEYS[k][4] << 8) | KEYS[k][5];
                mf_classic_set_key_found(mfc, s, MfClassicKeyTypeB, kv);
                if(!sec_key[s]) {
                    sec_key[s] = KEYS[k];
                    sec_kt[s] = CHAMELEON_MF_KEY_B;
                }
                keys_found++;
                break;
            }
        }
    }

    /* Read all block data with the sector's working key */
    for(uint16_t b = 0; b < total; b++) {
        if(abort_flag && *abort_flag) break;
        uint8_t sector = mf_classic_get_sector_by_block((uint8_t)b);
        if(!sec_key[sector]) continue;
        uint8_t blk[16];
        if(chameleon_mf1_read_block((uint8_t)b, sec_kt[sector], sec_key[sector], blk)) {
            MfClassicBlock mb;
            memcpy(mb.data, blk, sizeof(mb.data));
            mf_classic_set_block_read(mfc, (uint8_t)b, &mb);
            read_ok++;
        }
    }

    ESP_LOGD(TAG, "MFC read: %u/%u blocks, %u keys", read_ok, total, keys_found);
    nfc_device_set_data(device, NfcProtocolMfClassic, mfc);
    mf_classic_free(mfc);
    return true;
}

static bool chameleon_nfc_read_mf_ultralight(
    NfcDevice* device,
    const uint8_t* uid,
    uint8_t uid_len,
    const uint8_t atqa[2],
    uint8_t sak,
    volatile bool* abort_flag) {
    MfUltralightData* mfu = mf_ultralight_alloc();

    /* GET_VERSION (0x60) → derive exact type; fall back to Origin */
    uint8_t ver[16];
    uint16_t ver_len = 0;
    const uint8_t get_ver[1] = {0x60};
    bool have_version = chameleon_hf14a_raw(get_ver, 1, ver, sizeof(ver), &ver_len) &&
                        ver_len >= sizeof(MfUltralightVersion);

    MfUltralightType type = MfUltralightTypeOrigin;
    if(have_version) {
        memcpy(&mfu->version, ver, sizeof(MfUltralightVersion));
        type = mf_ultralight_get_type_by_version(&mfu->version);
    }
    mfu->type = type;

    Iso14443_3aData* iso = mf_ultralight_get_base_data(mfu);
    iso14443_3a_set_uid(iso, uid, uid_len);
    iso14443_3a_set_atqa(iso, atqa);
    iso14443_3a_set_sak(iso, sak);

    uint16_t pages_total = have_version ? mf_ultralight_get_pages_total(type) : 256;
    if(pages_total == 0 || pages_total > MF_ULTRALIGHT_MAX_PAGE_NUM)
        pages_total = MF_ULTRALIGHT_MAX_PAGE_NUM;

    uint16_t pages_read = 0;
    for(uint16_t p = 0; p < pages_total; p++) {
        if(abort_flag && *abort_flag) break;
        uint8_t rd[2] = {0x30, (uint8_t)p};
        uint8_t resp[16];
        uint16_t rlen = 0;
        if(!chameleon_hf14a_raw(rd, sizeof(rd), resp, sizeof(resp), &rlen) || rlen < 4) {
            break; /* end of memory / not readable — partial like a normal read */
        }
        memcpy(mfu->page[p].data, resp, MF_ULTRALIGHT_PAGE_SIZE);
        pages_read = p + 1;
    }

    if(!have_version) {
        /* No GET_VERSION (Origin/NTAG203/UL) — total = what we could read */
        pages_total = pages_read ? pages_read : pages_total;
    }
    mfu->pages_read = pages_read;
    mfu->pages_total = pages_total;

    /* On NTAG/UL-EV1 the PWD page always reads back as 0, so a plain page
     * read is never "complete" (mf_ultralight_is_all_data_read) and the card
     * shows up as locked → the saved menu hides "Write" and offers "Unlock".
     * Mirror the poller's try-default-password: PWD_AUTH with FFFFFFFF and,
     * on success, store the default password + returned PACK into the config
     * page so the dump is considered fully read. */
    MfUltralightConfigPages* cfg = NULL;
    if(pages_read == pages_total && mf_ultralight_get_config_page(mfu, &cfg)) {
        const uint8_t pwd_auth[5] = {0x1B, 0xFF, 0xFF, 0xFF, 0xFF};
        uint8_t pack[8];
        uint16_t pack_len = 0;
        if(chameleon_hf14a_raw(pwd_auth, sizeof(pwd_auth), pack, sizeof(pack), &pack_len) &&
           pack_len >= 2) {
            memset(cfg->password.data, 0xFF, MF_ULTRALIGHT_AUTH_PASSWORD_SIZE);
            cfg->pack.data[0] = pack[0];
            cfg->pack.data[1] = pack[1];
            ESP_LOGD(TAG, "default PWD_AUTH ok, pack=%02X%02X", pack[0], pack[1]);
        }
    }

    ESP_LOGD(
        TAG,
        "MFU read: type=%d ver=%d pages %u/%u",
        type,
        have_version,
        pages_read,
        pages_total);
    nfc_device_set_data(device, NfcProtocolMfUltralight, mfu);
    mf_ultralight_free(mfu);
    return true;
}

static uint16_t chameleon_nfc_mfc_tagtype(MfClassicType type) {
    switch(type) {
    case MfClassicTypeMini:
        return CHAMELEON_TAG_MIFARE_MINI;
    case MfClassicType4k:
        return CHAMELEON_TAG_MIFARE_4K;
    default:
        return CHAMELEON_TAG_MIFARE_1K;
    }
}

static uint16_t chameleon_nfc_mfu_tagtype(MfUltralightType type) {
    switch(type) {
    case MfUltralightTypeNTAG213:
        return CHAMELEON_TAG_NTAG_213;
    case MfUltralightTypeNTAG215:
        return CHAMELEON_TAG_NTAG_215;
    case MfUltralightTypeNTAG216:
        return CHAMELEON_TAG_NTAG_216;
    case MfUltralightTypeMfulC:
        return CHAMELEON_TAG_MF0ICU2;
    case MfUltralightTypeUL11:
        return CHAMELEON_TAG_MF0UL11;
    case MfUltralightTypeUL21:
        return CHAMELEON_TAG_MF0UL21;
    default:
        return CHAMELEON_TAG_MF0ICU1; /* Origin / NTAG203 / UL */
    }
}

bool chameleon_nfc_emulate(NfcDevice* device) {
    furi_check(device);
    if(!chameleon_is_connected()) return false;

    NfcProtocol proto = nfc_device_get_protocol(device);
    const uint8_t slot = 8; /* least-intrusive default slot */
    uint8_t uid[10] = {0};
    uint8_t uid_len = 0;
    uint8_t atqa[2] = {0};
    uint8_t sak = 0;
    bool ok = false;

    if(proto == NfcProtocolMfClassic) {
        const MfClassicData* d = nfc_device_get_data(device, NfcProtocolMfClassic);
        if(!d) return false;
        Iso14443_3aData* iso = mf_classic_get_base_data(d);
        size_t ul = 0;
        const uint8_t* u = iso14443_3a_get_uid(iso, &ul);
        memcpy(uid, u, ul > 10 ? 10 : ul);
        uid_len = (uint8_t)ul;
        iso14443_3a_get_atqa(iso, atqa);
        sak = iso14443_3a_get_sak(iso);

        uint16_t blocks = mf_classic_get_total_block_num(d->type);
        uint8_t* buf = malloc((size_t)blocks * 16);
        for(uint16_t b = 0; b < blocks; b++)
            memcpy(&buf[(size_t)b * 16], d->block[b].data, 16);

        ok = chameleon_set_device_mode(CHAMELEON_MODE_EMULATOR) &&
             chameleon_slot_select(slot, chameleon_nfc_mfc_tagtype(d->type)) &&
             chameleon_mf1_eload(0, buf, blocks) &&
             chameleon_set_anticoll(uid, uid_len, atqa, sak) &&
             chameleon_slot_config_save();
        free(buf);
    } else if(proto == NfcProtocolMfUltralight) {
        const MfUltralightData* d = nfc_device_get_data(device, NfcProtocolMfUltralight);
        if(!d) return false;
        Iso14443_3aData* iso = mf_ultralight_get_base_data(d);
        size_t ul = 0;
        const uint8_t* u = iso14443_3a_get_uid(iso, &ul);
        memcpy(uid, u, ul > 10 ? 10 : ul);
        uid_len = (uint8_t)ul;
        iso14443_3a_get_atqa(iso, atqa);
        sak = iso14443_3a_get_sak(iso);

        uint16_t pages = d->pages_total ? d->pages_total : mf_ultralight_get_pages_total(d->type);
        if(pages == 0 || pages > MF_ULTRALIGHT_MAX_PAGE_NUM) pages = MF_ULTRALIGHT_MAX_PAGE_NUM;
        uint8_t* buf = malloc((size_t)pages * MF_ULTRALIGHT_PAGE_SIZE);
        for(uint16_t p = 0; p < pages; p++)
            memcpy(&buf[(size_t)p * MF_ULTRALIGHT_PAGE_SIZE], d->page[p].data, MF_ULTRALIGHT_PAGE_SIZE);

        ok = chameleon_set_device_mode(CHAMELEON_MODE_EMULATOR) &&
             chameleon_slot_select(slot, chameleon_nfc_mfu_tagtype(d->type)) &&
             chameleon_mfu_eload(0, buf, pages) &&
             chameleon_set_anticoll(uid, uid_len, atqa, sak) &&
             chameleon_slot_config_save();
        free(buf);
    } else {
        ESP_LOGW(TAG, "emulate: unsupported protocol %d", proto);
        return false;
    }

    ESP_LOGD(TAG, "emulate proto=%d -> %s", proto, ok ? "OK" : "FAIL");
    return ok;
}

static bool chameleon_nfc_write_mf_classic(const MfClassicData* d, volatile bool* abort_flag) {
    /* Authenticate with the TARGET card's current key (found via dictionary,
     * default FFFFFFFFFFFF on a blank card) — NOT the source dump's keys.
     * Per sector: write the data blocks, then the sector trailer LAST so a
     * blank card gets the source's NDEF/MAD keys + access bits and becomes
     * NDEF-readable. (Skipping the trailer left blank clones unformatted.) */
    static const uint8_t KEYS[][6] = {
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
        {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5},
        {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7},
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5},
    };
    const size_t KEY_NUM = sizeof(KEYS) / sizeof(KEYS[0]);

    uint8_t sectors = mf_classic_get_total_sectors_num(d->type);
    uint16_t written = 0, failed = 0;

    for(uint8_t s = 0; s < sectors; s++) {
        if(abort_flag && *abort_flag) break;
        uint8_t tb = mf_classic_get_sector_trailer_num_by_sector(s);
        uint8_t tmp[16];

        const uint8_t* tk = NULL;
        uint8_t tkt = 0;
        for(size_t k = 0; k < KEY_NUM && !tk; k++) {
            if(chameleon_mf1_read_block(tb, CHAMELEON_MF_KEY_A, KEYS[k], tmp)) {
                tk = KEYS[k];
                tkt = CHAMELEON_MF_KEY_A;
            }
        }
        for(size_t k = 0; k < KEY_NUM && !tk; k++) {
            if(chameleon_mf1_read_block(tb, CHAMELEON_MF_KEY_B, KEYS[k], tmp)) {
                tk = KEYS[k];
                tkt = CHAMELEON_MF_KEY_B;
            }
        }
        if(!tk) continue; /* target sector locked with unknown key */

        uint8_t count = (s < 32) ? 4 : 16;
        uint16_t first = (uint16_t)tb - count + 1;
        for(uint16_t b = first; b < first + count; b++) {
            if(b == tb) continue; /* trailer written last */
            /* Block 0 (UID) is written too: clones the UID on Gen2/CUID
             * magic cards, harmlessly fails on a normal (read-only) one. */
            if(chameleon_mf1_write_block((uint8_t)b, tkt, tk, d->block[b].data))
                written++;
            else
                failed++;
        }
        /* Trailer last: installs the source's keys + access bits */
        if(chameleon_mf1_write_block((uint8_t)tb, tkt, tk, d->block[tb].data))
            written++;
        else
            failed++;
    }

    ESP_LOGD(TAG, "MFC write: %u ok, %u failed", written, failed);
    return written > 0;
}

static bool chameleon_nfc_write_mf_ultralight(
    const MfUltralightData* d,
    volatile bool* abort_flag) {
    uint16_t pages = d->pages_read ? d->pages_read : d->pages_total;
    if(pages > MF_ULTRALIGHT_MAX_PAGE_NUM) pages = MF_ULTRALIGHT_MAX_PAGE_NUM;
    /* User data only: skip pages 0-3 (UID/lock/CC) and the trailing
     * config/lock/PWD/PACK pages to avoid bricking the card. */
    uint16_t last = (pages > 5) ? (uint16_t)(pages - 5) : 0;
    uint16_t written = 0, failed = 0;

    for(uint16_t p = 4; p < last; p++) {
        if(abort_flag && *abort_flag) break;
        if(chameleon_mfu_write_page((uint8_t)p, d->page[p].data))
            written++;
        else
            failed++;
    }
    ESP_LOGD(TAG, "MFU write: pages 4..%u, %u ok, %u failed", last, written, failed);
    return written > 0;
}

bool chameleon_nfc_write_card(NfcDevice* device, volatile bool* abort_flag) {
    furi_check(device);
    if(!chameleon_is_connected()) return false;
    if(!chameleon_set_device_mode(CHAMELEON_MODE_READER)) return false;

    uint8_t uid[10] = {0};
    uint8_t uid_len = 0;
    uint8_t atqa[2] = {0};
    uint8_t sak = 0;
    if(!chameleon_hf14a_scan(uid, &uid_len, atqa, &sak)) return false; /* no card */

    NfcProtocol proto = nfc_device_get_protocol(device);
    if(proto == NfcProtocolMfClassic) {
        const MfClassicData* d = nfc_device_get_data(device, NfcProtocolMfClassic);
        return d ? chameleon_nfc_write_mf_classic(d, abort_flag) : false;
    } else if(proto == NfcProtocolMfUltralight) {
        const MfUltralightData* d = nfc_device_get_data(device, NfcProtocolMfUltralight);
        return d ? chameleon_nfc_write_mf_ultralight(d, abort_flag) : false;
    }
    ESP_LOGW(TAG, "write: unsupported protocol %d", proto);
    return false;
}

bool chameleon_nfc_wipe_card(volatile bool* abort_flag) {
    if(!chameleon_is_connected()) return false;
    if(!chameleon_set_device_mode(CHAMELEON_MODE_READER)) return false;

    uint8_t uid[10] = {0};
    uint8_t uid_len = 0;
    uint8_t atqa[2] = {0};
    uint8_t sak = 0;
    if(!chameleon_hf14a_scan(uid, &uid_len, atqa, &sak)) return false; /* no card */
    if(sak == 0x00) {
        ESP_LOGW(TAG, "wipe: only MIFARE Classic supported");
        return false;
    }

    static const uint8_t KEYS[][6] = {
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
        {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5},
        {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7},
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5},
    };
    const size_t KEY_NUM = sizeof(KEYS) / sizeof(KEYS[0]);
    const uint8_t zero[16] = {0};
    /* default trailer: keyA FF*6, access FF 07 80, GPB 69, keyB FF*6 */
    const uint8_t deftr[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07,
                               0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    MfClassicType type = chameleon_nfc_mfc_type(sak);
    uint8_t sectors = mf_classic_get_total_sectors_num(type);
    uint16_t ok = 0, fail = 0;

    for(uint8_t s = 0; s < sectors; s++) {
        if(abort_flag && *abort_flag) break;
        uint8_t tb = mf_classic_get_sector_trailer_num_by_sector(s);
        uint8_t tmp[16];
        const uint8_t* ka = NULL;
        const uint8_t* kb = NULL;
        for(size_t k = 0; k < KEY_NUM && !ka; k++)
            if(chameleon_mf1_read_block(tb, CHAMELEON_MF_KEY_A, KEYS[k], tmp)) ka = KEYS[k];
        for(size_t k = 0; k < KEY_NUM && !kb; k++)
            if(chameleon_mf1_read_block(tb, CHAMELEON_MF_KEY_B, KEYS[k], tmp)) kb = KEYS[k];
        if(!ka && !kb) continue;

        uint8_t count = (s < 32) ? 4 : 16;
        uint16_t first = (uint16_t)tb - count + 1;
        for(uint16_t b = first; b < first + count; b++) {
            if(b == 0) continue; /* keep manufacturer/UID block */
            const uint8_t* data = (b == tb) ? deftr : zero;
            /* NDEF access often permits trailer write only with key B —
             * try key B first, then key A. */
            bool w = false;
            if(kb) w = chameleon_mf1_write_block((uint8_t)b, CHAMELEON_MF_KEY_B, kb, data);
            if(!w && ka)
                w = chameleon_mf1_write_block((uint8_t)b, CHAMELEON_MF_KEY_A, ka, data);
            if(w)
                ok++;
            else
                fail++;
        }
    }
    ESP_LOGD(TAG, "MFC wipe: %u ok, %u failed", ok, fail);
    return ok > 0;
}

bool chameleon_nfc_read_card(NfcDevice* device, volatile bool* abort_flag) {
    furi_check(device);
    if(!chameleon_is_connected()) return false;

    if(!chameleon_set_device_mode(CHAMELEON_MODE_READER)) {
        ESP_LOGW(TAG, "set reader mode failed");
        return false;
    }

    uint8_t uid[10] = {0};
    uint8_t uid_len = 0;
    uint8_t atqa[2] = {0};
    uint8_t sak = 0;
    if(!chameleon_hf14a_scan(uid, &uid_len, atqa, &sak)) {
        return false; /* no card in field */
    }
    ESP_LOGD(TAG, "card uid_len=%u sak=%02X atqa=%02X%02X", uid_len, sak, atqa[0], atqa[1]);

    /* SAK 0x00 (ATQA 0x0044) → MIFARE Ultralight / NTAG; else MIFARE Classic */
    bool is_ultralight = (sak == 0x00);
    if(is_ultralight) {
        return chameleon_nfc_read_mf_ultralight(
            device, uid, uid_len, atqa, sak, abort_flag);
    }
    return chameleon_nfc_read_mf_classic(device, uid, uid_len, atqa, sak, abort_flag);
}
