#pragma once

/*
 * ESP32 port glue for I2C Tools CLI.
 *
 * This firmware has no furi_hal_i2c implementation (components/furi_hal/furi_hal_i2c.h is a
 * header-only stub and nothing is exported to FAPs), so i2c_compat.c provides the handful of
 * furi_hal_i2c_* calls the upstream sources use, on top of the ESP-IDF legacy i2c driver that
 * the firmware already runs on the external (Qwiic) bus. The upstream scanner/sender/CLI code
 * is unchanged.
 */

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_i2c.h> // not pulled in by furi_hal.h in this port
#include <boards/board.h>

/* ---- Bus pins -------------------------------------------------------------------------
 * paradoxt: GPIO47 (SDA) / GPIO48 (SCL), I2C_NUM_0 - the Qwiic/PN532 connector. The BQ25896
 * charger (0x6B) and BQ27220 fuel gauge (0x55) are on this same bus, so they show up in a scan. */
#if defined(BOARD_PIN_EXT_I2C_SDA) && defined(BOARD_PIN_EXT_I2C_SCL)
#define I2C_BUS_SDA_PIN BOARD_PIN_EXT_I2C_SDA
#define I2C_BUS_SCL_PIN BOARD_PIN_EXT_I2C_SCL
#elif defined(BOARD_PIN_QWIIC_SDA) && defined(BOARD_PIN_QWIIC_SCL)
#define I2C_BUS_SDA_PIN BOARD_PIN_QWIIC_SDA
#define I2C_BUS_SCL_PIN BOARD_PIN_QWIIC_SCL
#else
#error "I2C Tools CLI: this board defines no external I2C pins"
#endif

#ifdef BOARD_EXT_I2C_PORT
#define I2C_BUS_PORT BOARD_EXT_I2C_PORT
#else
#define I2C_BUS_PORT I2C_NUM_0
#endif

/* ---- Sniffer pins ---------------------------------------------------------------------
 * The sniffer reconfigures its pads as plain GPIO inputs, which detaches the I2C peripheral
 * from them. Doing that on the Qwiic pads would starve the fuel gauge / charger polling and
 * the NFC reader, so it taps two free header pins instead (GPIO4 = SDA, GPIO5 = SCL). */
#if defined(BOARD_PIN_EXT_GPIO_1) && defined(BOARD_PIN_EXT_GPIO_2)
#define I2C_SNIFF_SDA_PIN BOARD_PIN_EXT_GPIO_1
#define I2C_SNIFF_SCL_PIN BOARD_PIN_EXT_GPIO_2
#else
#define I2C_SNIFF_SDA_PIN I2C_BUS_SDA_PIN
#define I2C_SNIFF_SCL_PIN I2C_BUS_SCL_PIN
#endif

extern const GpioPin i2c_compat_sniff_sda;
extern const GpioPin i2c_compat_sniff_scl;

/* ---- Power-IC write guard -------------------------------------------------------------
 * Writes to the board's power ICs are refused: a stray register write to the BQ25896 can
 * enter ship mode / change charge limits, which this firmware treats as untouchable. Reads
 * (and the scan) are fine. */
#define I2C_ADDR_BQ25896 0x6B
#ifdef BQ27220_ADDR
#define I2C_ADDR_BQ27220 BQ27220_ADDR
#else
#define I2C_ADDR_BQ27220 0x55
#endif

/** Call once before the first bus use / CLI registration, and balance with _deinit(). */
void i2c_compat_init(void);
void i2c_compat_deinit(void);

/** True if the last tx/trx was refused by the power-IC write guard (not a bus error). */
bool i2c_compat_write_blocked(void);
