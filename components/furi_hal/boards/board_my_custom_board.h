/**
 * @file board_my_custom_board.h
 * Custom ESP32-S3 board with dedicated buttons (6-way pad + select/back).
 *
 * Display:  ST7789-compatible panel via SPI (320x172)
 * Input:    6-way button pad (Up/Down/Left/Right + Select/Back)
 * SubGHz:   CC1101 via shared SPI bus
 * NRF24:    On shared SPI bus with CC1101
 * SD:       On shared SPI bus with display
 * I2C:      NFC + external devices
 * UART:     RFID reader
 */

#pragma once

/* ---- Board metadata ---- */
#define BOARD_NAME        "Custom ESP32-S3 Board"
#define BOARD_TARGET      "esp32s3"

/* ---- Hardware Button Pins ---- */
#define BOARD_PIN_BUTTON_BOOT   0    /* Select / OK */
#define BOARD_PIN_BUTTON_KEY    21   /* Back / Kembali */
#define BOARD_PIN_BTN_UP        41   /* Up */
#define BOARD_PIN_BTN_DOWN      40   /* Down */
#define BOARD_PIN_BTN_LEFT      39   /* Left */
#define BOARD_PIN_BTN_RIGHT     38   /* Right */
#define BOARD_PIN_BATTERY_ADC   UINT16_MAX  /* No battery ADC */

/* ---- LCD Pins (ST7789-style panel via SPI) ---- */
#define BOARD_PIN_LCD_MOSI      18   /* SPI MOSI */
#define BOARD_PIN_LCD_SCLK      17   /* SPI SCLK */
#define BOARD_PIN_LCD_DC        15   /* Data/Command */
#define BOARD_PIN_LCD_CS        7    /* Chip Select */
#define BOARD_PIN_LCD_RST       16   /* Reset */
#define BOARD_PIN_LCD_BL        6    /* Backlight PWM */

/* ---- LCD Display Configuration (320x172 ST7789-compatible) ---- */
#define BOARD_LCD_H_RES         320     /* Native width after swap_xy */
#define BOARD_LCD_V_RES         172     /* Native height after swap_xy */
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_SPI_FREQ_HZ   (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS      8
#define BOARD_LCD_PARAM_BITS    8
#define BOARD_LCD_SWAP_XY       true
#define BOARD_LCD_MIRROR_X      false
#define BOARD_LCD_MIRROR_Y      false
#define BOARD_LCD_INVERT_COLOR  false   /* Adjust if colors are inverted */
#define BOARD_LCD_GAP_X         0
#define BOARD_LCD_GAP_Y         34
#define BOARD_LCD_BL_ACTIVE_LOW false   /* Backlight is active-high */
#define BOARD_LCD_COLOR_ORDER_BGR 0    /* Standard RGB order */

/* Flipper framebuffer → display color mapping (RGB565, byte-swapped for SPI) */
#define BOARD_LCD_FG_COLOR      0x20FD  /* Orange 0xFD20 byte-swapped */
#define BOARD_LCD_BG_COLOR      0x0000  /* Black */

/* ---- SD Card Pins (shared SPI bus with LCD) ---- */
#define BOARD_PIN_SD_CS         3
#define BOARD_PIN_SD_MISO       8

/* ---- Touch Controller — NOT PRESENT ---- */
#define BOARD_PIN_TOUCH_SCL     UINT16_MAX
#define BOARD_PIN_TOUCH_SDA     UINT16_MAX
#define BOARD_PIN_TOUCH_RST     UINT16_MAX
#define BOARD_PIN_TOUCH_INT     UINT16_MAX
#define BOARD_TOUCH_I2C_ADDR    0x00
#define BOARD_TOUCH_I2C_PORT    I2C_NUM_0
#define BOARD_TOUCH_I2C_FREQ_HZ 0
#define BOARD_TOUCH_I2C_TIMEOUT 0

/* ---- CC1101 (shared SPI bus with LCD + SD) ---- */
#define BOARD_PIN_CC1101_SCK    17      /* Shared with LCD_SCLK */
#define BOARD_PIN_CC1101_CSN    9
#define BOARD_PIN_CC1101_MISO   8       /* Shared with SD_MISO */
#define BOARD_PIN_CC1101_MOSI   18      /* Shared with LCD_MOSI */
#define BOARD_PIN_CC1101_GDO0   46      /* CC1101 GDO0 interrupt */
#define BOARD_PIN_CC1101_GDO2   UINT16_MAX
#define BOARD_PIN_CC1101_SW1    UINT16_MAX
#define BOARD_PIN_CC1101_SW0    UINT16_MAX
#define BOARD_CC1101_SPI_SHARED 1       /* CC1101 shares SPI2_HOST with LCD+SD */

/* ---- NRF24L01 (shared SPI bus) ---- */
#define BOARD_PIN_NRF24_SCK     17      /* Shared with LCD_SCLK */
#define BOARD_PIN_NRF24_MISO    8       /* Shared with SD_MISO */
#define BOARD_PIN_NRF24_MOSI    18      /* Shared with LCD_MOSI */
#define BOARD_PIN_NRF24_CSN     13
#define BOARD_PIN_NRF24_CE      14
#define BOARD_HAS_NRF24         1

/* ---- Infrared ---- */
#define BOARD_PIN_IR_TX         2
#define BOARD_PIN_IR_RX         1

/* ---- NFC / PN532 (via I2C) ---- */
#define BOARD_PIN_NFC_SCL       48
#define BOARD_PIN_NFC_SDA       47
#define BOARD_PIN_NFC_IRQ       UINT16_MAX
#define BOARD_PIN_NFC_RST       UINT16_MAX
#define BOARD_NFC_I2C_PORT      I2C_NUM_0

/* ---- Qwiic / External I2C (shared with NFC) ---- */
#define BOARD_PIN_QWIIC_SDA     47
#define BOARD_PIN_QWIIC_SCL     48

/* ---- RFID / RDM6300 (UART) ---- */
#define BOARD_PIN_RFID_RX       44
#define BOARD_PIN_RFID_TX       43
#define BOARD_RFID_UART_NUM     1

/* ---- Features ---- */
#define BOARD_HAS_TOUCH         0
#define BOARD_HAS_ENCODER       0      /* Using buttons instead */
#define BOARD_HAS_SD_CARD       1
#define BOARD_HAS_BLE           1
#define BOARD_HAS_RGB_LED       0      /* Not on this board */
#define BOARD_HAS_VIBRO         0      /* Not on this board */
#define BOARD_HAS_SPEAKER       0      /* Not on this board */
#define BOARD_HAS_IR            1
#define BOARD_HAS_IBUTTON       0
#define BOARD_HAS_RFID          1
#define BOARD_HAS_NFC           1
#define BOARD_HAS_SUBGHZ        1
#define BOARD_HAS_MIC           0      /* Not on this board */

/* ---- Power Management ---- */
#define BQ27220_ADDR                    0x55
#define BQ25896_CHARGE_LIMIT            1280
#define FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH (1300U)
