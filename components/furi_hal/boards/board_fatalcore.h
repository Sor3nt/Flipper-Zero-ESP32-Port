/**
 * @file board_fatalcore.h
 * Board definition: Fatal Core (FATALCORE BTW) — ESP32-S3 Flipper Zero clone
 *
 * MCU:      ESP32-S3-WROOM-1 N16R8 (16 MB flash, 8 MB octal PSRAM)
 * Display:  ST7789 320x172 via SPI (shared bus)
 * Input:    6 push buttons (Up/Down/Left/Right/Ok/Back-Power), active low
 * SubGHz:   CC1101 (AS07-M1101S) via SPI, GDO0 only
 * NRF24:    E01-2G4M27SX via SPI (shared, CS-muxed)
 * NFC:      ST25R3916 via SPI (CS + IRQ)            [driver: see furi_hal_nfc_st25r3916.c]
 * SD Card:  microSD via SPI (shared bus)
 * IR:       TSOP4838 RX + 2x IR LED TX
 * RGB LED:  1x WS2812B
 * Power:    TP4056 charger (3 kΩ PROG = 400 mA) + INA219 fuel gauge on I2C
 *           with a 50 mΩ (1206) shunt
 * iButton:  1-Wire on GPIO10
 *
 * Hardware reference (schematics, BOM, pinout, case): the vendor repository
 * https://github.com/Teapot174/fatalcore — hardware/fatalcore_pinout.txt and
 * hardware/fatalcore_BOM.txt are the source of truth for the pin map and for
 * the component values used by the drivers below.
 *
 * Bus layout (single SPI2_HOST, CS-muxed):
 *   SCK  = 17
 *   MOSI = 18
 *   MISO = 8
 *   LCD_CS=7  CC1101_CS=9  NRF24_CSN=13  NFC_CS=11  SD_CS=43
 *
 * GPIO 43/44 are UART0 TX/RX by default on the ESP32-S3. This board uses
 * GPIO43 for SD_CS and GPIO44 for the WS2812, so sdkconfig.defaults.fatalcore
 * moves the console to USB-Serial-JTAG (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG).
 */

#pragma once

/* ---- Board metadata ---- */
#define BOARD_NAME   "Fatalcore"
#define BOARD_ID     "fatalcore"
#define BOARD_TARGET "esp32s3"

/* ---- Buttons (active low, wired to GND) ----
 * Fatal Core: DOWN = GPIO40, LEFT = GPIO39 (verified on hardware; do not swap). */
#define BOARD_PIN_BTN_UP     41
#define BOARD_PIN_BTN_DOWN   40
#define BOARD_PIN_BTN_LEFT   39
#define BOARD_PIN_BTN_RIGHT  38
#define BOARD_PIN_BTN_OK     0  /* OK/Select — also the BOOT strapping pin */
#define BOARD_PIN_BTN_BACK   21 /* Escape/Power */

#define BOARD_PIN_BUTTON_BOOT   BOARD_PIN_BTN_OK
#define BOARD_PIN_BUTTON_KEY    BOARD_PIN_BTN_BACK
#define BOARD_PIN_BUTTON_UP     BOARD_PIN_BTN_UP
#define BOARD_PIN_BUTTON_DOWN   BOARD_PIN_BTN_DOWN
#define BOARD_PIN_BUTTON_LEFT   BOARD_PIN_BTN_LEFT
#define BOARD_PIN_BUTTON_RIGHT  BOARD_PIN_BTN_RIGHT
#define BOARD_PIN_ENCODER_BTN   BOARD_PIN_BTN_OK

/* Real 4-way D-pad with separate Up/Down/Left/Right buttons, not the T-Embed
 * 1-axis encoder: disable the encoder adaptations in the GUI (arrow remapping
 * in ViewInputModeLeftRight views, the on-screen keyboard transpose and the
 * "press Ok to edit" dance in variable_item_list) so every arrow means what
 * it says, including in the text input keyboard. */
#define BOARD_INPUT_DPAD 1

/* No battery ADC — the pack is monitored through the INA219 on I2C. */
#define BOARD_PIN_BATTERY_ADC   UINT16_MAX

/* ---- LCD (ST7789, 320x172, SPI2) ---- */
#define BOARD_PIN_LCD_MOSI 18
#define BOARD_PIN_LCD_SCLK 17
#define BOARD_PIN_LCD_DC   15
#define BOARD_PIN_LCD_CS   7
#define BOARD_PIN_LCD_RST  16
#define BOARD_PIN_LCD_BL   6

#define BOARD_LCD_H_RES 320
#define BOARD_LCD_V_RES 172
#define BOARD_LCD_SPI_HOST SPI2_HOST
/* 80 MHz instead of the usual 40: the UI pushes ~110 KB per full frame, so the
 * clock directly caps the frame rate (40 MHz ≈ 22 ms/frame of pure transfer).
 * The panel sits on a short flex cable and the ESP-IDF SPI master holds the
 * timing, so the ST7789 handles this fine; drop back to 40 MHz if a specific
 * unit shows pixel artefacts. */
/* 40 MHz: the ST7789 is only specified up to ~15 MHz for writes and the
 * panel sits on a flex cable shared with four other devices. 80 MHz made the
 * panel init/data path marginal on some boots (blank screen until a full power
 * cycle); 40 MHz is the fastest clock that has been reliable here. */
#define BOARD_LCD_SPI_FREQ_HZ (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS 8
#define BOARD_LCD_PARAM_BITS 8
#define BOARD_LCD_SWAP_XY true
#define BOARD_LCD_MIRROR_X true
#define BOARD_LCD_MIRROR_Y false
#define BOARD_LCD_INVERT_COLOR true
#define BOARD_LCD_GAP_X 0
#define BOARD_LCD_GAP_Y 34
#define BOARD_LCD_BL_ACTIVE_LOW false
#define BOARD_LCD_COLOR_ORDER_BGR false
#define BOARD_LCD_FG_COLOR 0xA0FD
#define BOARD_LCD_FG_COLOR_RB 0x5F03
#define BOARD_LCD_BG_COLOR 0x0000

/* ---- microSD (shared SPI bus) ---- */
#define BOARD_PIN_SD_CS   43
#define BOARD_PIN_SD_MISO 8

/* Shared-bus SD hardening. The card, the LCD, the CC1101, the nRF24 and the
 * ST25R3916 all sit on SPI2, so the card is the slowest and most fragile
 * device on the bus. BOARD_SD_RECOVERY turns on the former T-Embed-Plus
 * recovery path (mutex-protected init, bounded retries at 20/10/4 MHz, false
 * tiny-card identity rejection) without requiring the Plus Kconfig symbol.
 * BOARD_EARLY_SD_INIT enters SPI mode before the radios and LCD start
 * clocking the bus. BOARD_SD_SPI_FREQ_KHZ caps the bus clock. */
#define BOARD_SD_RECOVERY     1
#define BOARD_EARLY_SD_INIT   1
#define BOARD_SD_SPI_FREQ_KHZ 10000
/* Bring-up diagnostic: measure an idle MISO (internal pull-up/pull-down and a
 * short drive-high) right after mount. Tells an unresponsive ready-poll apart
 * from a chip holding the shared line down. Toggles the pull resistors on a
 * shared bus line, so it stays off outside bring-up. */
/* #define BOARD_SD_MISO_PROBE 1 */

/* ---- Touch: not present ---- */
#define BOARD_PIN_TOUCH_SCL UINT16_MAX
#define BOARD_PIN_TOUCH_SDA UINT16_MAX
#define BOARD_PIN_TOUCH_RST UINT16_MAX
#define BOARD_PIN_TOUCH_INT UINT16_MAX
#define BOARD_TOUCH_I2C_ADDR 0x00
#define BOARD_TOUCH_I2C_PORT I2C_NUM_0
#define BOARD_TOUCH_I2C_FREQ_HZ 0
#define BOARD_TOUCH_I2C_TIMEOUT 0

/* ---- CC1101 (shared SPI bus) ---- */
#define BOARD_PIN_CC1101_SCK  17
#define BOARD_PIN_CC1101_CSN  9
#define BOARD_PIN_CC1101_MISO 8
#define BOARD_PIN_CC1101_MOSI 18
#define BOARD_PIN_CC1101_GDO0 42
#define BOARD_PIN_CC1101_GDO2 UINT16_MAX
#define BOARD_PIN_CC1101_SW1  UINT16_MAX
#define BOARD_PIN_CC1101_SW0  UINT16_MAX
#define BOARD_CC1101_SPI_SHARED 1

/* ---- NRF24L01 (E01-2G4M27SX, shared SPI bus) ---- */
#define BOARD_PIN_NRF24_SCK  17
#define BOARD_PIN_NRF24_MISO 8
#define BOARD_PIN_NRF24_MOSI 18
#define BOARD_PIN_NRF24_CSN  13
#define BOARD_PIN_NRF24_CE   14
#define BOARD_HAS_NRF24      1

/* ---- Power enable: hard switch (MSK12C02), no GPIO ---- */
#define BOARD_PIN_PWR_EN UINT16_MAX

/* ---- Infrared ---- */
#define BOARD_PIN_IR_TX 2  /* 2x IR LED via MMBT2222A */
#define BOARD_PIN_IR_RX 1  /* TSOP4838 */

/* ---- NFC: ST25R3916 over SPI ----
 * The stock furi_hal_nfc.c is a PN532-over-I2C implementation. Setting
 * BOARD_NFC_USE_ST25R3916 selects the ST25R3916 SPI front-end instead
 * (components/furi_hal/furi_hal_nfc_st25r3916.c + the st25r3916 driver).
 */
#define BOARD_PIN_NFC_CS   11
#define BOARD_PIN_NFC_IRQ  12
#define BOARD_PIN_NFC_RST  UINT16_MAX /* no dedicated reset line on this board */
#define BOARD_NFC_SPI_HOST SPI2_HOST
#define BOARD_NFC_USE_ST25R3916 1
/* TEMPORARY bring-up switch: read IC_IDENTITY at several SPI clocks right
 * after reset to find out whether the id corruption (0x27/0x00 vs 0x2A) is a
 * clock-margin problem rather than a wrong chip. Costs a deinit/init cycle and
 * ~15 ms of boot time per clock, so it stays off outside bring-up. */
/* #define BOARD_NFC_CHIP_ID_SWEEP 1 */
/* TEMPORARY bring-up switch: probe the ST25R3916 at the very start of
 * furi_hal_init(), before the SD card and the display have touched SPI2.
 * Tells a chip that is already dead at power-up apart from one that the boot
 * sequence (or a previous firmware run) puts into a bad state. */
/* #define BOARD_NFC_EARLY_PROBE 1 */
/* TEMPORARY bring-up switch: read IC_IDENTITY with plain GPIO toggling (no
 * ESP-IDF SPI driver at all) plus a chip-select liveness check. Distinguishes
 * "our SPI setup is wrong" from "the front-end does not answer on the bus".
 * Runs at ~5 kHz and adds ~35 ms to boot, so enable it by hand only. */
/* #define BOARD_NFC_BITBANG_PROBE 1 */
/* TEMPORARY bring-up switch: when the chip id read fails, check whether the
 * chip executes commands at all by watching its IRQ line for the "direct
 * command termination" and "oscillator stable" events. A working MISO is not
 * needed for either. Runs once, only on the failing path (~45 ms). */
#define BOARD_NFC_EXEC_PROBE 0
/* TEMPORARY bring-up switch: measure the IRQ pin voltage with the internal
 * pull-up enabled to tell an actively driven low line from one that is merely
 * held by a resistor. Answers "is the front-end powered at all". */
/* #define BOARD_NFC_IRQ_ADC_PROBE 1 */

/* Diagnostic: log a system heartbeat and one task-state dump so an intermittent
 * UI freeze can be told apart from a full system wedge in the serial log. */
/* #define BOARD_HEARTBEAT_DEBUG 1 */

/* Diagnostic: rewrite one sector with its own contents right after mount to
 * verify the SD write path (busy-wait on an unusable MISO line). Never enable
 * this in a build that is handed to a user: it writes to the card on every
 * boot. */
/* #define BOARD_SD_WRITE_SELFTEST 1 */

/* Legacy PN532/I2C macros — kept so shared code that references them still
 * compiles; the ST25R3916 front-end does not use this bus. */
#define BOARD_PIN_NFC_SCL 48
#define BOARD_PIN_NFC_SDA 47
#define BOARD_PIN_NFC_TX  UINT16_MAX
#define BOARD_PIN_NFC_RX  UINT16_MAX
#define BOARD_NFC_I2C_PORT I2C_NUM_0

/* ---- I2C bus (INA219 @0x40, spare Qwiic header) ---- */
#define BOARD_PIN_QWIIC_SDA 47
#define BOARD_PIN_QWIIC_SCL 48
#define BOARD_I2C_PORT I2C_NUM_0
#define BOARD_INA219_ADDR 0x40

/* ---- iButton / 1-Wire ---- */
#define BOARD_PIN_IBUTTON 10

/* ---- WS2812B ---- */
#define BOARD_PIN_WS2812_DATA 44
#define BOARD_WS2812_LED_COUNT 1

/* ---- Features ---- */
#define BOARD_HAS_TOUCH    0
#define BOARD_HAS_ENCODER  0
#define BOARD_HAS_SD_CARD  1
#define BOARD_HAS_BLE      1
#define BOARD_HAS_RGB_LED  1
#define BOARD_HAS_VIBRO    0
#define BOARD_HAS_SPEAKER  0
#define BOARD_HAS_IR       1
#define BOARD_HAS_IBUTTON  1
#define BOARD_HAS_RFID     0
#define BOARD_HAS_NFC      1
#define BOARD_HAS_SUBGHZ   1
#define BOARD_HAS_MIC      0

#define BOARD_PIN_SPEAKER_BCLK UINT16_MAX
#define BOARD_PIN_SPEAKER_WCLK UINT16_MAX
#define BOARD_PIN_SPEAKER_DOUT UINT16_MAX
#define BOARD_PIN_RFID_RX UINT16_MAX
#define BOARD_PIN_RFID_TX UINT16_MAX
#define BOARD_RFID_UART_NUM 1

/* ---- Battery / charger (TP4056 has no I2C; INA219 is the gauge) ---- */
/* Shunt on the board: 50 mΩ (1206, 1 W) per the vendor BOM. Using the common
 * 100 mΩ calibration halves every current reading, which is exactly what the
 * first builds showed (charging reported ≈229 mA while the TP4056, programmed
 * with a 3 kΩ PROG resistor, delivers ≈400 mA). */
#define BOARD_INA219_SHUNT_MOHM 50
/* One 1000 mAh cell (vendor README/BOM). */
#define FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH (1000U)
#define BQ25896_CHARGE_LIMIT 1024
#define BQ27220_ADDR 0x55
/* BQ27220 probe on the Qwiic bus is expected to fail on this board; the
 * INA219 driver (components/furi_hal/furi_hal_ina219.c) is the real source. */