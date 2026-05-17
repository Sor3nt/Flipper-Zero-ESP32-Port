/**
 * @file chameleon_nfc.h
 * @brief Bridge: read a card via ChameleonUltra into a Flipper NfcDevice so
 *        the existing ReadSuccess / Save / parser pipeline can consume it.
 */
#pragma once

#include <stdbool.h>
#include <nfc/nfc_device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Switch the connected ChameleonUltra to reader mode, scan a 14443-A card
 * and read it. The card class is chosen from the SAK: MIFARE Classic
 * (Mini/1K/4K, default keys — Phase 2) or MIFARE Ultralight / NTAG
 * (Phase 3). On success the result is stored into @p device with the
 * appropriate protocol so the normal pipeline handles it.
 *
 * @param device     target NfcDevice (filled on success)
 * @param abort_flag optional, polled to bail out early (may be NULL)
 * @returns true if a card was scanned and stored.
 */
bool chameleon_nfc_read_card(NfcDevice* device, volatile bool* abort_flag);

/**
 * Load a saved card (@p device, MIFARE Classic or Ultralight/NTAG) into a
 * ChameleonUltra slot and switch the device to emulator mode.
 *
 * @returns true if the card was uploaded and emulation started.
 */
bool chameleon_nfc_emulate(NfcDevice* device);

/**
 * Write a saved card (@p device) onto the physical card currently in the
 * ChameleonUltra's field (reader mode). MIFARE Classic: data blocks only
 * (block 0 and sector trailers are skipped). Ultralight/NTAG: user data
 * pages only (UID / lock / CC / config pages skipped).
 *
 * @returns true if a card was found and the writable area was written.
 */
bool chameleon_nfc_write_card(NfcDevice* device, volatile bool* abort_flag);

/**
 * Reset the MIFARE Classic card currently in the ChameleonUltra's field to a
 * factory-default blank: zero data blocks + default sector trailers
 * (key FFFFFFFFFFFF, access FF0780, GPB 69). Block 0 (UID) is left intact.
 *
 * @returns true if a card was found and at least partly wiped.
 */
bool chameleon_nfc_wipe_card(volatile bool* abort_flag);

#ifdef __cplusplus
}
#endif
