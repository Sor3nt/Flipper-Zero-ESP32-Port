/**
 * @file board_m5stack_cardputer_adv.h
 * Board definition: M5Stack Cardputer-ADV (TCA8418 I2C keyboard)
 *
 * MCU:      ESP32-S3FN8 (dual-core Xtensa LX7, 8 MB Flash, NO PSRAM)
 * Display:  ST7789V2 240×135 RGB565 via SPI2
 * Input:    TCA8418 I2C Keyboard Controller (SDA=8, SCL=9, INT=11)
 * SD Card:  SPI3 (separate bus from LCD — MOSI=14, MISO=39, SCLK=40, CS=12)
 * IR:       TX (GPIO44) + RX (GPIO1)
 * Audio:    ES8311 codec (I2S + I2C)
 * Motion:   BMI270 IMU (I2C)
 */

#pragma once

/* ---- Board metadata ---- */
#define BOARD_NAME        "M5Stack Cardputer-ADV"
#define BOARD_TARGET      "esp32s3"

/* ---- LCD (ST7789V2) via SPI2 ---- */
#define BOARD_PIN_LCD_MOSI      35
#define BOARD_PIN_LCD_SCLK      36
#define BOARD_PIN_LCD_CS        37
#define BOARD_PIN_LCD_DC        34
#define BOARD_PIN_LCD_RST       33
#define BOARD_PIN_LCD_BL        38
#define BOARD_PIN_LCD_SPI_MISO  (-1)

/* ---- LCD Display Configuration ---- */
#define BOARD_LCD_H_RES         240
#define BOARD_LCD_V_RES         135
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_SPI_FREQ_HZ   (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS      8
#define BOARD_LCD_PARAM_BITS    8
#define BOARD_LCD_SWAP_XY       true
#define BOARD_LCD_MIRROR_X      true
#define BOARD_LCD_MIRROR_Y      false
#define BOARD_LCD_INVERT_COLOR  true
/* ST7789V2 1.14" panel (135x240 native) in Rotation 1 (MADCTL = MV|MX).
 * The esp_lcd driver swaps x/y BEFORE adding gap, so after swap:
 *   gap_x → column offset (short 135px axis) = 40
 *   gap_y → row offset    (long  240px axis) = 53
 * These values come from the standard ST7789 135x240 rotation table. */
#define BOARD_LCD_GAP_X         40
#define BOARD_LCD_GAP_Y         53
#define BOARD_LCD_BL_ACTIVE_LOW false
#define BOARD_LCD_COLOR_ORDER_BGR false

#define BOARD_LCD_FG_COLOR      0xA0FD
#define BOARD_LCD_BG_COLOR      0x0000

/* ---- SD Card via SPI3 ---- */
#define BOARD_HAS_SD            1
#define BOARD_HAS_SD_CARD       1
#define BOARD_PIN_SD_CS         12
#define BOARD_PIN_SD_MOSI       14
#define BOARD_PIN_SD_MISO       39
#define BOARD_PIN_SD_SCLK       40
#define BOARD_SD_SPI_HOST       SPI3_HOST

/* ---- TCA8418 Keyboard (I2C) ---- */
#define BOARD_HAS_TCA8418       1
#define KB_I2C_PORT             I2C_NUM_0
#define KB_I2C_FREQ_HZ          400000
#define KB_PIN_SDA              8
#define KB_PIN_SCL              9
#define KB_PIN_INT              11
#define KB_I2C_ADDR             0x34
/* Aliases expected by furi_hal_i2c_bus.c */
#define KB_I2C_PIN_SDA          KB_PIN_SDA
#define KB_I2C_PIN_SCL          KB_PIN_SCL

/* TCA8418 registers & bits (needed by target_input.c) */
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

/* Key IDs (Navigation) — aligned with hardware manual */
#define KB_ADV_KEY_OK           0x43   /* Enter (67) */
#define KB_ADV_KEY_BACK         0x41   /* Del */
#define KB_ADV_KEY_UP           0x39   /* Fn+; (57) */
#define KB_ADV_KEY_DOWN         0x3A   /* Fn+. (58) */
#define KB_ADV_KEY_LEFT         0x36   /* Fn+, (54) */
#define KB_ADV_KEY_RIGHT        0x40   /* Fn+/ (64) */

/* Unmapped Dummy Keys from log to prevent warnings */
#define KB_ADV_KEY_DUMMY_1      0x01
#define KB_ADV_KEY_DUMMY_4      0x04
#define KB_ADV_KEY_DUMMY_D      0x0D
#define KB_ADV_KEY_DUMMY_11     0x11
#define KB_ADV_KEY_DUMMY_17     0x17
#define KB_ADV_KEY_DUMMY_1B     0x1B

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

/* ---- SubGHz / CC1101 (EXT 14P Bus) ---- */
#define BOARD_PIN_CC1101_SCK    40
#define BOARD_PIN_CC1101_MISO   39
#define BOARD_PIN_CC1101_MOSI   14
#define BOARD_PIN_CC1101_CSN    13
#define BOARD_PIN_CC1101_GDO0   5
#define BOARD_PIN_CC1101_GDO2   UINT16_MAX
#define BOARD_PIN_CC1101_SW1    UINT16_MAX
#define BOARD_PIN_CC1101_SW0    UINT16_MAX
/* SubGHz/CC1101 + SD + NRF24 all live on the EXT pins 14/39/40 = the SPI3
 * hardware host, NOT SPI2 (that's the LCD's host — sharing it leaves the panel
 * blank). Put CC1101 on SPI3, shared with the SD via CS-mux (CC1101 CS=13,
 * SD CS=12). BOARD_CC1101_SPI_SHARED stays 0 (it specifically means "shares
 * SPI2 with the LCD", which would re-break the display). DO NOT set it to 1. */
#define BOARD_CC1101_SPI_SHARED 0
#define BOARD_SUBGHZ_SPI_HOST   SPI3_HOST

/* ---- NRF24L01 (EXT 14P Bus) ---- */
#define BOARD_HAS_NRF24         1
#define BOARD_PIN_NRF24_SCK     40
#define BOARD_PIN_NRF24_MISO    39
#define BOARD_PIN_NRF24_MOSI    14
#define BOARD_PIN_NRF24_CSN     6
#define BOARD_PIN_NRF24_CE      4

/* ---- Power Enable — NOT PRESENT ---- */
#define BOARD_PIN_PWR_EN        UINT16_MAX

/* ---- Audio (ES8311 via I2S) ---- */
#define BOARD_HAS_ES8311        1
#define BOARD_PIN_I2S_SCLK      41
#define BOARD_PIN_I2S_LRCK      43
#define BOARD_PIN_I2S_DOUT      42
#define BOARD_PIN_I2S_MCLK      0
/* ES8311 control interface: I2C. Confirmed by boot-time i2c scan — the codec
 * ACKs at 0x18 on the internal keyboard I2C bus (GPIO8/9 = I2C_NUM_0, shared
 * with the TCA8418 keyboard @0x34 and BMI270 IMU @0x69). */
#define BOARD_ES8311_I2C_PORT   I2C_NUM_0
#define BOARD_ES8311_I2C_ADDR   0x18

/* FuriHalSpeaker expects BOARD_PIN_SPEAKER_* */
#define BOARD_PIN_SPEAKER_BCLK  BOARD_PIN_I2S_SCLK
#define BOARD_PIN_SPEAKER_WCLK  BOARD_PIN_I2S_LRCK
#define BOARD_PIN_SPEAKER_DOUT  BOARD_PIN_I2S_DOUT

/* ---- IMU (BMI270 via I2C) ---- */
#define BOARD_HAS_BMI270        1
#define BMI270_I2C_ADDR         0x68

/* ---- Miscellaneous ---- */
#define BOARD_PIN_BUTTON_BOOT   0
#define BOARD_PIN_BATTERY_ADC   10

#define BOARD_PIN_IR_TX         44
#define BOARD_PIN_IR_RX         1

#define BOARD_PIN_MIC_DATA      46
#define BOARD_PIN_MIC_CLK       43

/* ---- WS2812 RGB LED — PRESENT ---- */
#define BOARD_PIN_WS2812_DATA   21
#define BOARD_WS2812_LED_COUNT  1
#define BOARD_HAS_RGB_LED       1

/* ---- RFID / RDM6300 — NOT PRESENT ---- */
#define BOARD_PIN_RFID_RX       UINT16_MAX
#define BOARD_PIN_RFID_TX       UINT16_MAX
#define BOARD_RFID_UART_NUM     1

/* ---- NFC / RFID2 (WS1850S or PN532) via Grove I2C ---- */
#define BOARD_PIN_NFC_SCL       BOARD_PIN_QWIIC_SCL
#define BOARD_PIN_NFC_SDA       BOARD_PIN_QWIIC_SDA
#define BOARD_PIN_NFC_IRQ       UINT16_MAX
#define BOARD_PIN_NFC_RST       UINT16_MAX
#define BOARD_NFC_I2C_PORT      I2C_NUM_1
#define BOARD_NFC_I2C_ADDR      0x28  /* WS1850S default; same as PN532 */

/* ---- ST25R3916 NFC frontend (M5Stack NFC Universal Unit) via same Grove I2C ----
 * The ST25R3916 is on the SAME Grove I2C bus as the PN532 (BOARD_NFC_I2C_PORT),
 * but at 7-bit I2C address 0x50 (100K/400K). The Grove 4-pin connector exposes
 * NO IRQ line, so ST25R3916 events must be polled from its IRQ status registers
 * (that is Task 2+; Task 1 is register read/write + chip-ID only). Keeping the
 * PN532 defines above intact so that backend stays selectable. */
#define BOARD_HAS_ST25R3916       1
#define BOARD_ST25R3916_I2C_ADDR  0x50

/* ---- Grove / Qwiic I2C ---- */
#define BOARD_PIN_QWIIC_SDA     2
#define BOARD_PIN_QWIIC_SCL     1
#define I2C_SDA_GPIO            BOARD_PIN_QWIIC_SDA
#define I2C_SCL_GPIO            BOARD_PIN_QWIIC_SCL

/* ---- Feature flags ---- */
#define BOARD_HAS_TOUCH         0
#define BOARD_HAS_ENCODER       0
#define BOARD_HAS_BLE           1
#define BOARD_HAS_VIBRO         0
#define BOARD_HAS_SPEAKER       1
#define BOARD_HAS_IR            1
#define BOARD_HAS_IR_TX         1
#define BOARD_HAS_IR_RX         1
#define FURI_HAL_SPEAKER_GPIO   BOARD_PIN_SPEAKER_DOUT

#define BOARD_HAS_IBUTTON       0
#define BOARD_HAS_RFID          0
#define BOARD_HAS_NFC           1  /* WS1850S/PN532 via Grove HY2.0-4P */
#define BOARD_HAS_SUBGHZ        1
#define BOARD_HAS_MIC           1
#define BOARD_HAS_IMU           1

/* ---- Battery ---- */
#define BQ27220_ADDR            0x00
#define BQ_I2C_PORT             I2C_NUM_0
#define BQ_I2C_SDA              UINT16_MAX
#define BQ_I2C_SCL              UINT16_MAX
/* The ADV has NO BQ27220/BQ25896 (it senses battery via ADC on GPIO10), so the
 * power HAL must NOT install an I2C driver on I2C_NUM_0 — that port belongs to
 * the TCA8418 keyboard (GPIO8/9). Without this, furi_hal_power seizes I2C_NUM_0
 * with the wrong pins and the keyboard's i2c_driver_install fails (dead keys). */
#define BOARD_POWER_SKIP_I2C
#define HIGH_DRAIN_CURRENT_THRESHOLD (-200)
#define FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH     (1750U)
#define BQ25896_CHARGE_LIMIT    1280
#define FURI_HAL_POWER_ADC_DIVIDER_RATIO        (2.0f)