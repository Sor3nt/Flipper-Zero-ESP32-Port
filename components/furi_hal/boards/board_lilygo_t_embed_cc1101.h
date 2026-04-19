/**
 * @file board_lilygo_t_embed_cc1101.h
 * Board definition: LilyGo T-Embed CC1101
 *
 * MCU:      ESP32-S3 (dual-core Xtensa LX7)
 * Display:  ST7789 320x170 RGB565 via SPI
 * Input:    Rotary Encoder (A/B/Button) + Key button
 * SubGHz:   CC1101 via SPI (shared bus with LCD + SD)
 * NFC:      PN532 via I2C
 * SD Card:  SPI (shared bus with LCD + CC1101)
 * IR:       TX (IO02) + RX (IO01)
 * Speaker:  I2S DAC (IO46/IO40/IO07)
 * RGB LED:  WS2812 x8 (IO14)
 * Mic:      I2S PDM (IO42/IO39)
 */

#pragma once

/* ---- Board metadata ---- */
#define BOARD_NAME        "LilyGo T-Embed CC1101"
#define BOARD_TARGET      "esp32s3"

/* ---- Hardware Button / Encoder Pins ---- */
#define BOARD_PIN_BUTTON_BOOT   0   /* Encoder button (BOOT/IO00, active low) */
#define BOARD_PIN_BUTTON_KEY    6   /* Side key button */
#define BOARD_PIN_BATTERY_ADC   UINT16_MAX  /* No battery ADC on this board */

/* ---- Rotary Encoder ---- */
#define BOARD_PIN_ENCODER_A     4
#define BOARD_PIN_ENCODER_B     5
#define BOARD_PIN_ENCODER_BTN   0   /* Same as BOOT button */

/* ---- LCD Pins (ST7789 via SPI) ---- */
#define BOARD_PIN_LCD_MOSI      9
#define BOARD_PIN_LCD_SCLK      11
#define BOARD_PIN_LCD_DC        16
#define BOARD_PIN_LCD_CS        41
#define BOARD_PIN_LCD_RST       40
#define BOARD_PIN_LCD_BL        21  /* Backlight PWM */

/* ---- LCD Display Configuration ---- */
#define BOARD_LCD_H_RES         320     /* Native width after swap_xy */
#define BOARD_LCD_V_RES         170     /* Native height after swap_xy */
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_SPI_FREQ_HZ   (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS      8
#define BOARD_LCD_PARAM_BITS    8
#define BOARD_LCD_SWAP_XY       true
#define BOARD_LCD_MIRROR_X      false   /* 180° rotated vs Waveshare */
#define BOARD_LCD_MIRROR_Y      true    /* 180° rotated vs Waveshare */
#define BOARD_LCD_INVERT_COLOR  true    /* T-Embed ST7789 uses display inversion ON */
#define BOARD_LCD_GAP_X         0       /* TODO: verify gap on hardware */
#define BOARD_LCD_GAP_Y         35      /* TODO: verify gap on hardware */
#define BOARD_LCD_BL_ACTIVE_LOW false   /* Backlight is active-high */
#define BOARD_LCD_COLOR_ORDER_BGR false /* RGB order with esp_lcd driver */

/* Flipper framebuffer → display color mapping (RGB565, native byte order) */
#define BOARD_LCD_FG_COLOR      0xA0FD  /* Flipper Orange 0xFDA0 byte-swapped for S3 SPI */
#define BOARD_LCD_FG_COLOR_RB   0x5F03  /* Same orange with R/B swapped (0x035F swapped) — for post-flash BGR state */
#define BOARD_LCD_BG_COLOR      0x0000  /* Black */

/* ---- SD Card Pins (shared SPI bus with LCD and CC1101) ---- */
#define BOARD_PIN_SD_CS         13
#define BOARD_PIN_SD_MISO       10

/* ---- Touch Controller — NOT PRESENT (rotary encoder input) ---- */
#define BOARD_PIN_TOUCH_SCL     UINT16_MAX
#define BOARD_PIN_TOUCH_SDA     UINT16_MAX
#define BOARD_PIN_TOUCH_RST     UINT16_MAX
#define BOARD_PIN_TOUCH_INT     UINT16_MAX
#define BOARD_TOUCH_I2C_ADDR    0x00
#define BOARD_TOUCH_I2C_PORT    I2C_NUM_0
#define BOARD_TOUCH_I2C_FREQ_HZ 0
#define BOARD_TOUCH_I2C_TIMEOUT 0

/* ---- SubGHz / CC1101 (shared SPI bus with LCD + SD) ---- */
#define BOARD_PIN_CC1101_SCK    11      /* Shared with LCD_CLK and SD_SCLK */
#define BOARD_PIN_CC1101_CSN    12      /* LORA_CS */
#define BOARD_PIN_CC1101_MISO   10      /* Shared with SD_MISO */
#define BOARD_PIN_CC1101_MOSI   9       /* Shared with LCD_MOSI and SD_MOSI */
#define BOARD_PIN_CC1101_GDO0   3       /* LORA_IO0 — CC1101 GDO0 interrupt */
#define BOARD_PIN_CC1101_GDO2   38      /* LORA_IO2 — CC1101 GDO2 */
#define BOARD_PIN_CC1101_SW1    47      /* RF band switch: see table below */
#define BOARD_PIN_CC1101_SW0    48      /* RF band switch: see table below */
/* Band selection: SW1=H SW0=L → 315MHz, SW1=L SW0=H → 868/915MHz, SW1=H SW0=H → 434MHz */
#define BOARD_CC1101_SPI_SHARED 1       /* CC1101 shares SPI2_HOST with LCD+SD (CS-muxed) */

/* ---- Power Enable (controls CC1101 + WS2812 power) ---- */
#define BOARD_PIN_PWR_EN        15      /* Must be HIGH to power CC1101 and WS2812 */

/* ---- IR ---- */
#define BOARD_PIN_IR_TX         2       /* IR_EN */
#define BOARD_PIN_IR_RX         1       /* IR_RX */

/* ---- NFC / PN532 (via I2C) ---- */
#define BOARD_PIN_NFC_SCL       18
#define BOARD_PIN_NFC_SDA       8
#define BOARD_PIN_NFC_IRQ       17
#define BOARD_PIN_NFC_RST       45      /* PN532_RF_RES */
#define BOARD_NFC_I2C_PORT      I2C_NUM_0

/* ---- Speaker (I2S) ---- */
#define BOARD_PIN_SPEAKER_BCLK  46
#define BOARD_PIN_SPEAKER_WCLK  40
#define BOARD_PIN_SPEAKER_DOUT  7

/* ---- WS2812 RGB LED Strip ---- */
#define BOARD_PIN_WS2812_DATA   14
#define BOARD_WS2812_LED_COUNT  8

/* ---- Microphone (I2S PDM) ---- */
#define BOARD_PIN_MIC_DATA      42
#define BOARD_PIN_MIC_CLK       39

/* ---- Qwiic / External I2C (shared with NFC) ---- */
#define BOARD_PIN_QWIIC_SDA     8       /* Shared with NFC SDA */
#define BOARD_PIN_QWIIC_SCL     18      /* Shared with NFC SCL */

/* ---- Features ---- */
#define BOARD_HAS_TOUCH         0
#define BOARD_HAS_ENCODER       1
#define BOARD_HAS_SD_CARD       1
#define BOARD_HAS_BLE           1
#define BOARD_HAS_RGB_LED       1
#define BOARD_HAS_VIBRO         0
#define BOARD_HAS_SPEAKER       1
#define BOARD_HAS_IR            1
#define BOARD_HAS_IBUTTON       0
#define BOARD_HAS_RFID          1

/* ---- RFID / RDM6300 (UART, 125kHz read-only) ---- */
#define BOARD_PIN_RFID_RX       44      /* GROVE / SERIAL_RX */
#define BOARD_PIN_RFID_TX       43      /* GROVE / SERIAL_TX */
#define BOARD_RFID_UART_NUM     1       /* UART1 */
#define BOARD_HAS_NFC           1
#define BOARD_HAS_SUBGHZ        1       /* Built-in CC1101 */
#define BOARD_HAS_MIC           1

/* Battery Charger */
#define BQ27220_ADDR            0x55
#define BQ_I2C_PORT     I2C_NUM_0
#define BQ_I2C_SDA      BOARD_PIN_QWIIC_SDA
#define BQ_I2C_SCL      BOARD_PIN_QWIIC_SCL
#define HIGH_DRAIN_CURRENT_THRESHOLD (-200)
#define FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH     (1300U)
#define BQ25896_CHARGE_LIMIT  1280
