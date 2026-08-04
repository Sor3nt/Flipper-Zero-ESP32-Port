#pragma once

// M5Stack Unit RFID2 driver — WS1850S (MFRC522-register-compatible), 13.56 MHz,
// I2C @ 0x28. Reads ISO14443-A card UIDs (MIFARE Classic/Ultralight, NTAG).
//
// Connects to the Cardputer-ADV side Grove PORT (HY2.0): SDA=G2, SCL=G1 — a
// SEPARATE I2C bus from the internal keyboard bus (G8/G9). The driver initializes
// this bus itself on a free I2C peripheral. (G1 is shared with IR-RX, so don't use
// IR while the RFID2 is plugged in.)
//
// NOT hardware-tested — standard MFRC522 sequences; validate on the unit.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <driver/i2c.h>

#include <boards/board.h> // BOARD_PIN_QWIIC_* Grove pins (board-specific)

#define RFID2_I2C_ADDR 0x28
#define RFID2_I2C_PORT I2C_NUM_1 // dedicated bus for the Grove port
// Grove I2C pins come from the board's QWIIC/Grove defines so this driver lands
// on the right connector per board (Cardputer-ADV → G2/G1; StickS3 → G9/G10, which
// keeps it OFF the StickS3 radio pins G2=CC1101_CSN / G1=nRF24_CE). Falls back to
// the Cardputer-ADV Grove pins on boards that don't define QWIIC.
#if defined(BOARD_PIN_QWIIC_SDA) && defined(BOARD_PIN_QWIIC_SCL)
#define RFID2_SDA_PIN  BOARD_PIN_QWIIC_SDA
#define RFID2_SCL_PIN  BOARD_PIN_QWIIC_SCL
#else
#define RFID2_SDA_PIN  2 // Cardputer-ADV Grove SDA (G2)
#define RFID2_SCL_PIN  1 // Cardputer-ADV Grove SCL (G1)
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
