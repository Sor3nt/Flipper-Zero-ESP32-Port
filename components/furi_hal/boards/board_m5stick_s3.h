/**
 * @file board_m5stick_s3.h
 * Board definition: M5Stack StickS3 (ESP32-S3-PICO-1-N8R8)
 *
 * MCU:      ESP32-S3 (dual-core Xtensa LX7, 8 MB Flash, 8 MB PSRAM)
 * Display:  ST7789P3 135x240 RGB565 via SPI2 (MOSI=39 SCK=40 DC=45 CS=41 RST=21 BL=38)
 * Input:    TWO physical buttons only — KEY1=GPIO11, KEY2=GPIO12 (no keyboard)
 * Storage:  NO microSD — internal-flash LittleFS mounted at /sdcard (BOARD_HAS_SD 0)
 * IR:       TX=GPIO46, RX=GPIO42 (native)
 * Audio:    ES8311 codec (I2S MCLK=18 BCLK=17 LRCK=15 DOUT=14 DIN=16) — Phase 4
 * Motion:   BMI270 IMU (I2C SDA=47 SCL=48) — Phase 1 (tilt nav)
 * PMIC:     M5PM1 (I2C 0x6e) — stubbed via BOARD_POWER_SKIP_I2C
 * Radios:   WiFi/BLE built-in. The pingequa RF Pack S3 2-in-1 puts both radios
 *           on SPI3 (SCK=5 MISO=4 MOSI=6), CS-muxed: CC1101 SubGHz CSN=2/GDO0=3
 *           and nRF24 CSN=8/CE=1. No NFC.
 *
 * PHASE 0 bring-up: display + 2-button input + LittleFS + IR + BLE. SubGHz/nRF24/
 * NFC/audio/IMU/RGB gated OFF (added in later phases). See SCOPE_m5stick_s3_port.md.
 */

#pragma once

/* ---- Board metadata ---- */
#define BOARD_NAME        "M5Stack StickS3"
#define BOARD_TARGET      "esp32s3"

/* ---- LCD (ST7789P3, 135x240 portrait) via SPI2 ----
 * Reuse the Cardputer-ADV ST7789 rotation/gap approach as a starting point; the
 * P3 panel may need offset/rotation tweaks on-device (see scope §7 risks). */
#define BOARD_PIN_LCD_MOSI      39
#define BOARD_PIN_LCD_SCLK      40
#define BOARD_PIN_LCD_CS        41
#define BOARD_PIN_LCD_DC        45
#define BOARD_PIN_LCD_RST       21
#define BOARD_PIN_LCD_BL        38
#define BOARD_PIN_LCD_SPI_MISO  (-1)

/* ---- LCD Display Configuration ---- */
#define BOARD_LCD_H_RES         240
#define BOARD_LCD_V_RES         135
#define BOARD_LCD_SPI_HOST      SPI2_HOST
/* 20 MHz — matches Bruce's known-good StickS3 config. 40 MHz (the Cardputer value)
 * is too aggressive for this panel's routing: it displayed once on a freshly-reset
 * panel, then went black on every re-init as the ST7789 commands got corrupted at
 * the marginal clock. */
#define BOARD_LCD_SPI_FREQ_HZ   (20 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS      8
#define BOARD_LCD_PARAM_BITS    8
#define BOARD_LCD_SWAP_XY       true
#define BOARD_LCD_MIRROR_X      true
#define BOARD_LCD_MIRROR_Y      false
#define BOARD_LCD_INVERT_COLOR  true
#define BOARD_LCD_GAP_X         40
#define BOARD_LCD_GAP_Y         53
#define BOARD_LCD_BL_ACTIVE_LOW false
#define BOARD_LCD_COLOR_ORDER_BGR false

#define BOARD_LCD_FG_COLOR      0xA0FD
#define BOARD_LCD_BG_COLOR      0x0000

/* Phase-0 diagnostic: fill the panel R/G/B/W at boot to verify the panel + pins
 * can display anything (independent of the GUI). Disabled — display confirmed
 * working; the Stick now boots straight to the GUI with no color-flash test. */
/* #define BOARD_DISPLAY_SELFTEST  1 */

/* The StickS3's ST7789P3 needs the full power/VCOM/porch/gamma init (what M5GFX
 * sends); esp_lcd's minimal ST7789 init leaves it blank. See furi_hal_display.c. */
#define BOARD_ST7789_FULL_INIT  1

/* ---- Storage: NO SD card — LittleFS on internal flash at /sdcard ----
 * BOARD_HAS_SD 0 routes furi_hal_sd_mount() to the LittleFS backend. The SD pin
 * macros below are defined only so furi_hal_sd.c compiles; they are never driven.
 * BOARD_SD_SPI_HOST is intentionally NOT defined (keeps SD off the LCD's SPI2). */
#define BOARD_HAS_SD            0
#define BOARD_HAS_SD_CARD       0
#define BOARD_PIN_SD_CS         UINT16_MAX
#define BOARD_PIN_SD_MOSI       UINT16_MAX
#define BOARD_PIN_SD_MISO       UINT16_MAX
#define BOARD_PIN_SD_SCLK       UINT16_MAX

/* ---- Input: two GPIO buttons (no I2C keyboard) ---- */
#define BOARD_HAS_TCA8418       0
#define BOARD_PIN_BTN_KEY1      11   /* front/top button */
#define BOARD_PIN_BTN_KEY2      12   /* side button */

/* Two physical buttons only. KEY1 (front): tap=Ok, hold 0.6s=Left, hold 1.2s=Right.
 * KEY2 (side): tap=Down, double=Up, long=Back. KEY1's holds produce real Left/Right,
 * so button rows and value sliders are reachable. BOARD_TWO_BUTTON_INPUT is kept as a
 * belt-and-braces convenience: Widget/DialogEx button rows (which fire only on
 * InputKeyLeft/Right) ALSO accept Down as the Left button and Ok as the Right button,
 * so a confirmation like "Exit to X Menu?" is never a dead end. Guarded so full-keypad
 * boards (e.g. T-Embed) keep stock behavior. */
#define BOARD_TWO_BUTTON_INPUT  1

/* BMI270 IMU on the internal I2C bus provides tilt -> Left/Right nav. Boards with this
 * set expose the tilt-nav toggle in System settings and the target_input tilt setter. */
#define BOARD_HAS_TILT_NAV      1

/* Internal I2C bus (BMI270 IMU + ES8311 codec + M5PM1 PMIC live here). Named with
 * the KB_* aliases furi_hal_i2c_bus.c expects for I2C_NUM_0, even though there is
 * no keyboard on this board. */
#define KB_I2C_PORT             I2C_NUM_0
#define KB_I2C_FREQ_HZ          400000
#define KB_PIN_SDA              47
#define KB_PIN_SCL              48
#define KB_PIN_INT              UINT16_MAX
#define KB_I2C_ADDR             0x00
#define KB_I2C_PIN_SDA          KB_PIN_SDA
#define KB_I2C_PIN_SCL          KB_PIN_SCL

/* TCA8418 register defs kept for source compatibility (unused: no TCA8418). */
#define TCA8418_REG_CFG         0x01
#define TCA8418_REG_INT_STAT    0x02
#define TCA8418_REG_KEY_LCK_EC  0x03
#define TCA8418_REG_KEY_EVENT_A 0x04
#define TCA8418_REG_KP_GPIO1    0x1D
#define TCA8418_REG_KP_GPIO2    0x1E
#define TCA8418_REG_KP_GPIO3    0x1F
#define TCA8418_REG_DEBOUNCE1   0x29
#define TCA8418_REG_DEBOUNCE2   0x2A
#define TCA8418_REG_DEBOUNCE3   0x2B
#define TCA8418_CFG_KE_IEN      (1 << 0)
#define TCA8418_CFG_INT_CFG     (1 << 1)
#define TCA8418_CFG_OVR_FLOW_M  (1 << 2)
#define TCA8418_KEY_PRESS_MASK  0x80
#define TCA8418_KEY_ID_MASK     0x7F
#define TCA8418_FIFO_EMPTY      0x00

/* ---- Encoder / Rotary input — NOT PRESENT ---- */
#define BOARD_PIN_ENCODER_A     UINT16_MAX
#define BOARD_PIN_ENCODER_B     UINT16_MAX
#define BOARD_PIN_ENCODER_BTN   UINT16_MAX
#define BOARD_PIN_BUTTON_KEY    UINT16_MAX

/* ---- Touch Controller — NOT PRESENT ---- */
#define BOARD_PIN_TOUCH_SCL     UINT16_MAX
#define BOARD_PIN_TOUCH_SDA     UINT16_MAX
#define BOARD_PIN_TOUCH_RST     UINT16_MAX
#define BOARD_PIN_TOUCH_INT     UINT16_MAX
#define BOARD_TOUCH_I2C_ADDR    0x00
#define BOARD_TOUCH_I2C_PORT    I2C_NUM_0
#define BOARD_TOUCH_I2C_FREQ_HZ 0
#define BOARD_TOUCH_I2C_TIMEOUT 0

/* ---- SubGHz / CC1101 on the SPI3 radio bus (pingequa RF Pack S3 2-in-1) ----
 * Pin map matches Bruce's m5stack-sticks3 defaults (known-good on this
 * hardware). This board has no SD card (/ext lives on internal flash), so SPI3
 * is dedicated to the radios — no CS-mux with SD, unlike the Cardputer ADV.
 * GDO2/SW0/SW1 are not wired on the RF Pack. */
#define BOARD_PIN_CC1101_SCK    5
#define BOARD_PIN_CC1101_MISO   4
#define BOARD_PIN_CC1101_MOSI   6
#define BOARD_PIN_CC1101_CSN    2
#define BOARD_PIN_CC1101_GDO0   3
#define BOARD_PIN_CC1101_GDO2   UINT16_MAX
#define BOARD_PIN_CC1101_SW1    UINT16_MAX
#define BOARD_PIN_CC1101_SW0    UINT16_MAX
/* Specifically means "shares the LCD's bus" — the LCD is on SPI2, so 0. */
#define BOARD_CC1101_SPI_SHARED 0
#define BOARD_SUBGHZ_SPI_HOST   SPI3_HOST

/* ---- NRF24L01 on the SPI3 radio bus (pingequa RF Pack S3 2-in-1) ----
 * Parallel-attached alongside the CC1101 and CS-muxed: both share
 * furi_hal_spi_bus_subghz, whose recursive mutex serializes them. The nRF24
 * inherits the bus pins from the CC1101 defines (furi_hal_resources.c builds
 * gpio_ext_pb3/pa6/pa7 from BOARD_PIN_CC1101_SCK/MISO/MOSI), so only CSN + CE
 * are actually consumed. SCK/MISO/MOSI below are documentation only — nothing
 * reads them; they are listed to record the physical wiring.
 * furi_hal.c's init_early drives CSN HIGH / CE LOW before any CC1101 traffic so
 * the nRF24 cannot snoop CC1101 transfers and corrupt the bus. */
#define BOARD_HAS_NRF24         1
#define BOARD_PIN_NRF24_SCK     5    /* unused: shared, see CC1101_SCK */
#define BOARD_PIN_NRF24_MISO    4    /* unused: shared, see CC1101_MISO */
#define BOARD_PIN_NRF24_MOSI    6    /* unused: shared, see CC1101_MOSI */
#define BOARD_PIN_NRF24_CSN     8
#define BOARD_PIN_NRF24_CE      1

/* ---- Power Enable — NOT PRESENT ---- */
#define BOARD_PIN_PWR_EN        UINT16_MAX

/* ---- M5PM1 PMIC on the internal I2C bus (GPIO47 SDA / GPIO48 SCL) ----
 * StickS3 uses the M5PM1 power-management chip (addr 0x6E). Its watchdog is
 * enabled by default; M5Unified/Bruce disable it at boot (reg 0x0A=0) + disable
 * I2C idle-sleep (0x09=0) so the PMIC stays configured and doesn't reset its
 * rails. The LCD rail itself hangs off M5PM1 GPIO2, so without this the panel is
 * unpowered and the screen stays dark even though the ST7789 + backlight PWM
 * report success. A build only appears to work without it when it inherits an
 * already-initialized PMIC state from Bruce/M5Launcher (survives soft reset, not
 * a real power cycle). */
#define BOARD_HAS_M5PM1         1
#define BOARD_PMIC_I2C_PORT     I2C_NUM_0
#define BOARD_PMIC_SDA          47
#define BOARD_PMIC_SCL          48
#define BOARD_PMIC_ADDR         0x6E

/* ---- Audio (ES8311) — Phase 4 (disabled in Phase 0) ----
 * Pins recorded for later; BOARD_HAS_SPEAKER/ES8311 = 0 for now. */
#define BOARD_HAS_ES8311        0
#define BOARD_PIN_I2S_MCLK      18
#define BOARD_PIN_I2S_SCLK      17
#define BOARD_PIN_I2S_LRCK      15
#define BOARD_PIN_I2S_DOUT      14
#define BOARD_PIN_I2S_DIN       16
#define BOARD_ES8311_I2C_PORT   I2C_NUM_0
#define BOARD_ES8311_I2C_ADDR   0x18
#define BOARD_PIN_SPEAKER_BCLK  BOARD_PIN_I2S_SCLK
#define BOARD_PIN_SPEAKER_WCLK  BOARD_PIN_I2S_LRCK
#define BOARD_PIN_SPEAKER_DOUT  BOARD_PIN_I2S_DOUT

/* ---- IMU (BMI270) — Phase 1 (disabled in Phase 0) ---- */
#define BOARD_HAS_BMI270        0
#define BMI270_I2C_ADDR         0x68

/* ---- Miscellaneous ---- */
#define BOARD_PIN_BUTTON_BOOT   0
#define BOARD_PIN_BATTERY_ADC   UINT16_MAX  /* battery via M5PM1 PMIC, not ADC */

#define BOARD_PIN_IR_TX         46
#define BOARD_PIN_IR_RX         42

#define BOARD_PIN_MIC_DATA      UINT16_MAX
#define BOARD_PIN_MIC_CLK       UINT16_MAX

/* ---- RGB LED — NOT PRESENT on StickS3 ---- */
#define BOARD_PIN_WS2812_DATA   UINT16_MAX
#define BOARD_WS2812_LED_COUNT  0
#define BOARD_HAS_RGB_LED       0

/* ---- RFID / RDM6300 — NOT PRESENT ---- */
#define BOARD_PIN_RFID_RX       UINT16_MAX
#define BOARD_PIN_RFID_TX       UINT16_MAX
#define BOARD_RFID_UART_NUM     1

/* ---- NFC — external I2C unit via Grove Port.A (M5Stack NFC Unit = ST25R3916
 * @0x50, or WS1850S/PN532). No onboard NFC; furi_hal_nfc probes PN532@0x24 then
 * falls back to the ST25R3916@0x50 backend. No IRQ line on Grove → polled. ---- */
#define BOARD_PIN_NFC_SCL       BOARD_PIN_QWIIC_SCL
#define BOARD_PIN_NFC_SDA       BOARD_PIN_QWIIC_SDA
#define BOARD_PIN_NFC_IRQ       UINT16_MAX
#define BOARD_PIN_NFC_RST       UINT16_MAX
#define BOARD_NFC_I2C_PORT      I2C_NUM_1
#define BOARD_NFC_I2C_ADDR      0x28
#define BOARD_HAS_ST25R3916     1
#define BOARD_ST25R3916_I2C_ADDR 0x50

/* ---- Grove Port.A / Qwiic I2C (external) — HY2.0-4P on G9/G10 ----
 * Orientation SDA=G9 / SCL=G10, confirmed by an on-device I2C scan (the WS1850S
 * RFID2 unit ACKs at 0x28 on this ordering, NOT the reverse). Grove 5V is gated
 * by the M5PM1 PMIC (reg 0x06 bit3), enabled in furi_hal_m5pm1_init — without
 * that the port is unpowered and nothing responds regardless of pins. */
#define BOARD_PIN_QWIIC_SDA     9
#define BOARD_PIN_QWIIC_SCL     10
#define I2C_SDA_GPIO            BOARD_PIN_QWIIC_SDA
#define I2C_SCL_GPIO            BOARD_PIN_QWIIC_SCL

/* ---- Feature flags (Phase 0) ---- */
#define BOARD_HAS_TOUCH         0
#define BOARD_HAS_ENCODER       0
#define BOARD_HAS_BLE           1
#define BOARD_HAS_VIBRO         0
#define BOARD_HAS_SPEAKER       0
#define BOARD_HAS_IR            1
#define BOARD_HAS_IR_TX         1
#define BOARD_HAS_IR_RX         1
#define FURI_HAL_SPEAKER_GPIO   BOARD_PIN_SPEAKER_DOUT

#define BOARD_HAS_IBUTTON       0
#define BOARD_HAS_RFID          0
#define BOARD_HAS_NFC           1  /* M5Stack NFC Unit (ST25R3916 @0x50) via Grove */
#define BOARD_HAS_SUBGHZ        1
#define BOARD_HAS_MIC           0
#define BOARD_HAS_IMU           0

/* ---- Battery / power: M5PM1 PMIC (I2C 0x6e) ----
 * Backlight (G38) is a plain GPIO, but the LCD's power rail is NOT: it hangs off
 * M5PM1 GPIO2, so the PMIC must be brought up before the display (see
 * BOARD_HAS_M5PM1 above). Only the I2C battery gauge is skipped here; the level
 * is reported virtually. */
#define BQ27220_ADDR            0x00
#define BQ_I2C_PORT             I2C_NUM_0
#define BQ_I2C_SDA              UINT16_MAX
#define BQ_I2C_SCL              UINT16_MAX
#define BOARD_POWER_SKIP_I2C
#define HIGH_DRAIN_CURRENT_THRESHOLD (-200)
#define FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH     (250U)
#define BQ25896_CHARGE_LIMIT    1280
#define FURI_HAL_POWER_ADC_DIVIDER_RATIO        (2.0f)
