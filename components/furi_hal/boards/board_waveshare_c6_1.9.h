/**
 * @file board_waveshare_c6_1.9.h
 * Board definition: Waveshare ESP32-C6-LCD-1.9 (Touch variant)
 *
 * Display:  ST7789V2 172x320 RGB565 via SPI
 * Touch:    CST816S via I2C
 * SD Card:  SPI (shared bus with display)
 */

#pragma once

/* ---- Board metadata ---- */
#define BOARD_NAME        "Waveshare ESP32-C6-LCD-1.9"
#define BOARD_TARGET      "esp32c6"

/* ---- Hardware Button Pins ---- */
#define BOARD_PIN_BUTTON_BOOT   9   /* BOOT button (active low) */
#define BOARD_PIN_BATTERY_ADC   0   /* BAT_ADC (VCC / 3) */

/* ---- LCD Pins (ST7789V2 via SPI) ---- */
#define BOARD_PIN_LCD_MOSI      4
#define BOARD_PIN_LCD_SCLK      5
#define BOARD_PIN_LCD_DC        6
#define BOARD_PIN_LCD_CS        7
#define BOARD_PIN_LCD_RST       14
#define BOARD_PIN_LCD_BL        15  /* Backlight PWM (active low via P-FET) */

/* ---- LCD Display Configuration ---- */
#define BOARD_LCD_H_RES         320     /* Native width after swap_xy */
#define BOARD_LCD_V_RES         172     /* Native height after swap_xy */
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_SPI_FREQ_HZ   (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS      8
#define BOARD_LCD_PARAM_BITS    8
#define BOARD_LCD_SWAP_XY       true
#define BOARD_LCD_MIRROR_X      true
#define BOARD_LCD_MIRROR_Y      false
#define BOARD_LCD_INVERT_COLOR  true
#define BOARD_LCD_GAP_X         0
#define BOARD_LCD_GAP_Y         34
#define BOARD_LCD_BL_ACTIVE_LOW true    /* Backlight via P-channel high-side switch */

/* Flipper framebuffer → display color mapping (RGB565, byte-swapped for SPI) */
#define BOARD_LCD_FG_COLOR      0x20FD  /* Orange 0xFD20 byte-swapped */
#define BOARD_LCD_BG_COLOR      0x0000  /* Black */

/* ---- SD Card Pins (shared SPI bus) ---- */
#define BOARD_PIN_SD_CS         20
#define BOARD_PIN_SD_MISO       19

/* ---- Touch Controller (CST816S via I2C) ---- */
#define BOARD_PIN_TOUCH_SCL     8
#define BOARD_PIN_TOUCH_SDA     18
#define BOARD_PIN_TOUCH_RST     21
#define BOARD_PIN_TOUCH_INT     22
#define BOARD_TOUCH_I2C_ADDR    0x15
#define BOARD_TOUCH_I2C_PORT    I2C_NUM_0
#define BOARD_TOUCH_I2C_FREQ_HZ 200000
#define BOARD_TOUCH_I2C_TIMEOUT 1000    /* ticks */

/* ---- SubGHz / CC1101 (external, active pin mapping) ---- */
#define BOARD_PIN_CC1101_SCK    2
#define BOARD_PIN_CC1101_CSN    1
#define BOARD_PIN_CC1101_MISO   17
#define BOARD_PIN_CC1101_MOSI   3
#define BOARD_PIN_CC1101_GDO0   23

/* ---- Features ---- */
#define BOARD_HAS_TOUCH         1
#define BOARD_HAS_SD_CARD       1
#define BOARD_HAS_BLE           1
#define BOARD_HAS_RGB_LED       0
#define BOARD_HAS_VIBRO         0
#define BOARD_HAS_SPEAKER       0
#define BOARD_HAS_IR            0
#define BOARD_HAS_IBUTTON       0
#define BOARD_HAS_RFID          0
#define BOARD_HAS_NFC           0
#define BOARD_HAS_SUBGHZ        0   /* External CC1101 possible but not built-in */

/* ---- Power Management (not present on this board) ---- */
#define BQ27220_ADDR                    0x55
#define BQ25896_CHARGE_LIMIT            1280
#define FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH (1300U)
