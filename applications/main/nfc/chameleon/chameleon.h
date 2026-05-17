/**
 * @file chameleon.h
 * @brief BLE client for an external ChameleonUltra device (Nordic UART Service).
 *
 * Phase 1: connect/disconnect + low-level command exchange + a couple of
 * device-info commands (app version, battery) to prove the BLE link.
 *
 * The transport is a Bluedroid GATT *client*. Since the firmware normally runs
 * a BLE *peripheral* (Flipper RPC), chameleon_connect() stops the BT service
 * stack first (mirrors applications/main/ble_spam/ble_walk_hal.c) and
 * chameleon_disconnect() restarts it.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ChameleonUltra command codes (subset; full set added in later phases) */
typedef enum {
    ChameleonCmdGetAppVersion = 1000,
    ChameleonCmdChangeMode = 1001,
    ChameleonCmdGetDeviceMode = 1002,
    ChameleonCmdSetActiveSlot = 1003,
    ChameleonCmdSetSlotTagType = 1004,
    ChameleonCmdSetSlotDataDefault = 1005,
    ChameleonCmdSetSlotEnable = 1006,
    ChameleonCmdSlotDataConfigSave = 1009,
    ChameleonCmdGetBatteryInfo = 1025,
    ChameleonCmdGetDeviceModel = 1033,
    ChameleonCmdHf14aScan = 2000,
    ChameleonCmdMf1ReadOneBlock = 2008,
    ChameleonCmdMf1WriteOneBlock = 2009,
    ChameleonCmdHf14aRaw = 2010,
    ChameleonCmdMf1WriteEmuBlockData = 4000,
    ChameleonCmdHf14aSetAntiCollData = 4001,
    ChameleonCmdMf0NtagWriteEmuPageData = 4022,
} ChameleonCmd;

/* ChameleonUltra response status bytes */
#define CHAMELEON_STATUS_HF_TAG_OK 0x00
#define CHAMELEON_STATUS_LF_TAG_OK 0x40
#define CHAMELEON_STATUS_SUCCESS   0x68

#define CHAMELEON_RESP_DATA_MAX 512

typedef struct {
    uint16_t command;
    uint8_t status;
    uint16_t data_len;
    uint8_t data[CHAMELEON_RESP_DATA_MAX];
} ChameleonResp;

/**
 * Stop the BT peripheral stack, scan for a device advertising as
 * "ChameleonUltra", connect, discover the NUS service and subscribe to
 * notifications. Blocking, abortable via the optional flag.
 *
 * @param abort_flag optional pointer polled during the (potentially long)
 *                   scan/connect; set true to bail out early. May be NULL.
 * @returns true if fully connected and ready for commands.
 */
bool chameleon_connect(volatile bool* abort_flag);

/** Disconnect the GATT link and restart the BT peripheral stack. */
void chameleon_disconnect(void);

bool chameleon_is_connected(void);

/**
 * Send a ChameleonUltra command and wait for its response.
 *
 * @param cmd      command code
 * @param data     payload (may be NULL if len == 0)
 * @param len      payload length
 * @param out      filled with the parsed response (may be NULL to ignore)
 * @param timeout_ms response wait timeout
 * @returns true if a response frame arrived before the timeout (check
 *          out->status for command-level success).
 */
bool chameleon_cmd(
    uint16_t cmd,
    const uint8_t* data,
    uint16_t len,
    ChameleonResp* out,
    uint32_t timeout_ms);

/* Convenience wrappers (Phase 1 sanity checks) */
bool chameleon_get_app_version(uint8_t* major, uint8_t* minor);
bool chameleon_get_battery(uint16_t* millivolt, uint8_t* percent);

/* ---- HF reader operations (Phase 2) ---- */

#define CHAMELEON_MODE_EMULATOR 0x00
#define CHAMELEON_MODE_READER   0x01

#define CHAMELEON_MF_KEY_A 0x60
#define CHAMELEON_MF_KEY_B 0x61

/** CHANGE_DEVICE_MODE to reader (required before HF14A scan/read). */
bool chameleon_set_device_mode(uint8_t mode);

/**
 * HF14A_SCAN: activate the type-A card in the field.
 * @returns true if a card answered (out params filled).
 */
bool chameleon_hf14a_scan(
    uint8_t* uid,
    uint8_t* uid_len,
    uint8_t atqa[2],
    uint8_t* sak);

/**
 * MF1_READ_ONE_BLOCK: authenticate (key A/B) and read one 16-byte block.
 * @param key_type CHAMELEON_MF_KEY_A or CHAMELEON_MF_KEY_B
 * @returns true on success (out16 filled with 16 bytes).
 */
bool chameleon_mf1_read_block(
    uint8_t block,
    uint8_t key_type,
    const uint8_t key[6],
    uint8_t out16[16]);

/**
 * HF14A_RAW: send a raw type-A frame (PN532-equivalent of a transceive).
 * Uses the standard option set (auto-select, append CRC, wait response,
 * check response CRC) — i.e. behaves like a normal ISO14443-3A exchange.
 *
 * @param data     frame bytes (e.g. {0x30,page} READ, {0x60} GET_VERSION)
 * @param len      frame length in bytes
 * @param out      response buffer
 * @param out_cap  capacity of @p out
 * @param out_len  filled with the response length
 * @returns true on HF_TAG_OK with data.
 */
bool chameleon_hf14a_raw(
    const uint8_t* data,
    uint8_t len,
    uint8_t* out,
    uint16_t out_cap,
    uint16_t* out_len);

/* ---- HF emulation operations (Phase 4) ---- */

/* ChameleonUltra TagType codes */
#define CHAMELEON_TAG_MIFARE_MINI 1000
#define CHAMELEON_TAG_MIFARE_1K   1001
#define CHAMELEON_TAG_MIFARE_2K   1002
#define CHAMELEON_TAG_MIFARE_4K   1003
#define CHAMELEON_TAG_NTAG_213    1100
#define CHAMELEON_TAG_NTAG_215    1101
#define CHAMELEON_TAG_NTAG_216    1102
#define CHAMELEON_TAG_MF0ICU1     1103 /* MIFARE Ultralight */
#define CHAMELEON_TAG_MF0ICU2     1104 /* MIFARE Ultralight C */
#define CHAMELEON_TAG_MF0UL11     1105
#define CHAMELEON_TAG_MF0UL21     1106
#define CHAMELEON_TAG_NTAG_210    1107
#define CHAMELEON_TAG_NTAG_212    1108

/** Enable HF on @p slot (1..8), make it active, set its tag type and
 * initialise the slot to defaults for that type (required before loading
 * emulation data). */
bool chameleon_slot_select(uint8_t slot, uint16_t tag_type);

/** Persist/apply the active slot configuration (call after loading data). */
bool chameleon_slot_config_save(void);

/** Upload MIFARE Classic emulation data (chunked), starting at @p start_block. */
bool chameleon_mf1_eload(uint8_t start_block, const uint8_t* data, uint16_t nblocks);

/** Upload MIFARE Ultralight / NTAG emulation pages (chunked). */
bool chameleon_mfu_eload(uint8_t start_page, const uint8_t* data, uint16_t npages);

/** Set the emulated anti-collision data (UID / ATQA / SAK). */
bool chameleon_set_anticoll(
    const uint8_t* uid,
    uint8_t uid_len,
    const uint8_t atqa[2],
    uint8_t sak);

/* ---- HF reader write operations (Phase 6) ---- */

/** MF1_WRITE_ONE_BLOCK: authenticate (key A/B) and write one 16-byte block. */
bool chameleon_mf1_write_block(
    uint8_t block,
    uint8_t key_type,
    const uint8_t key[6],
    const uint8_t data16[16]);

/** Write one 4-byte MIFARE Ultralight / NTAG page (WRITE cmd 0xA2). */
bool chameleon_mfu_write_page(uint8_t page, const uint8_t data4[4]);

#ifdef __cplusplus
}
#endif
