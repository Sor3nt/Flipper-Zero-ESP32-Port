#pragma once

// M5Stack Unit RFID2 driver — WS1850S (MFRC522-register-compatible), 13.56 MHz,
// I2C @ 0x28. Reads ISO14443-A card UIDs (MIFARE Classic/Ultralight, NTAG).
//
// Connects to the board's external/Qwiic I2C pins. This bus may be shared with
// power management and NFC, so the firmware owns its initialization.
//
// NOT hardware-tested — standard MFRC522 sequences; validate on the unit.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <driver/i2c.h>

#include <boards/board.h> // BOARD_PIN_QWIIC_* Grove pins (board-specific)

#define RFID2_I2C_ADDR 0x28
#if defined(BOARD_EXT_I2C_PORT)
#define RFID2_I2C_PORT BOARD_EXT_I2C_PORT
#elif defined(BOARD_PIN_QWIIC_SDA) && defined(BOARD_PIN_QWIIC_SCL) && \
    defined(BOARD_NFC_I2C_PORT)
#define RFID2_I2C_PORT BOARD_NFC_I2C_PORT
#else
#define RFID2_I2C_PORT I2C_NUM_1
#endif

#if defined(BOARD_PIN_QWIIC_SDA) && defined(BOARD_PIN_QWIIC_SCL)
#define RFID2_SDA_PIN  BOARD_PIN_QWIIC_SDA
#define RFID2_SCL_PIN  BOARD_PIN_QWIIC_SCL
#else
#define RFID2_SDA_PIN  2
#define RFID2_SCL_PIN  1
#endif

/** Soft-reset + configure the reader and turn the antenna on. false on failure. */
bool rfid2_init(void);

/** Read the MFRC522 VersionReg (sanity check the chip responds; 0x00/0xFF = no chip). */
uint8_t rfid2_version(void);

/** Diagnostic: probe every I2C address on the Grove bus; fill `out` with the 7-bit
 * addresses that ACK, return the count (or -1 if the bus can't be brought up). */
int rfid2_scan(uint8_t* out, int max);

/**
 * Poll once for a card in the field. If present, fills uid (4/7/10 bytes),
 * uid_len and sak, and returns true. Returns false if no card.
 */
bool rfid2_read_uid(uint8_t* uid, size_t uid_max, size_t* uid_len, uint8_t* sak);
