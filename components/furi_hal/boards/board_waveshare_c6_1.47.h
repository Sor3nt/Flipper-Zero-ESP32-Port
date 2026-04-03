/**
 * @file board_waveshare_c6_1.47.h
 * Board definition: Waveshare ESP32-C6-LCD-1.47
 *
 * Display:  ST7789V2 172x320 RGB565 via SPI
 * Touch:    CST816S via I2C (shared bus with QMI8658 IMU)
 * SD Card:  SPI (shared bus with display)
 * IMU:      QMI8658 via I2C (SDA=18, SCL=19, INT1=5, INT2=6)
 */

#pragma once

/* ---- Board metadata ---- */
#define BOARD_NAME        "Waveshare ESP32-C6-LCD-1.47"
#define BOARD_TARGET      "esp32c6"

/* ---- Hardware Button Pins ---- */
#define BOARD_PIN_BUTTON_BOOT   9   /* BOOT button (active low) */
#define BOARD_PIN_BATTERY_ADC   0   /* BAT_ADC (VCC / 3) */

/* ---- LCD Pins (ST7789V2 via SPI) ---- */
#define BOARD_PIN_LCD_MOSI      2   /* LCD_DIN */
#define BOARD_PIN_LCD_SCLK      1   /* LCD_CLK */
#define BOARD_PIN_LCD_DC        15
#define BOARD_PIN_LCD_CS        14
#define BOARD_PIN_LCD_RST       22
#define BOARD_PIN_LCD_BL        23  /* Backlight PWM */

/* ---- LCD Display Configuration ---- */
#define BOARD_LCD_H_RES         320     /* Native width after swap_xy */
#define BOARD_LCD_V_RES         172     /* Native height after swap_xy */
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_SPI_FREQ_HZ   (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS      8
#define BOARD_LCD_PARAM_BITS    8
#define BOARD_LCD_SWAP_XY       true
#define BOARD_LCD_MIRROR_X      false
#define BOARD_LCD_MIRROR_Y      false
#define BOARD_LCD_INVERT_COLOR  true
#define BOARD_LCD_GAP_X         0
#define BOARD_LCD_GAP_Y         34
#define BOARD_LCD_BL_ACTIVE_LOW false   /* Backlight is active-high (direct drive) */

/* Flipper framebuffer → display color mapping (RGB565, byte-swapped for SPI) */
#define BOARD_LCD_FG_COLOR      0x20FD  /* Orange 0xFD20 byte-swapped */
#define BOARD_LCD_BG_COLOR      0x0000  /* Black */

/* ---- SD Card Pins (shared SPI bus with LCD: MOSI=GPIO2, SCLK=GPIO1) ---- */
#define BOARD_PIN_SD_CS         4
#define BOARD_PIN_SD_MISO       3

/* ---- Touch Controller (CST816S via I2C, shared bus with QMI8658 IMU) ---- */
#define BOARD_PIN_TOUCH_SCL     19
#define BOARD_PIN_TOUCH_SDA     18
#define BOARD_PIN_TOUCH_RST     20
#define BOARD_PIN_TOUCH_INT     21
#define BOARD_TOUCH_I2C_ADDR    0x15
#define BOARD_TOUCH_I2C_PORT    I2C_NUM_0
#define BOARD_TOUCH_I2C_FREQ_HZ 200000
#define BOARD_TOUCH_I2C_TIMEOUT 1000    /* ticks */

/* ---- IMU (QMI8658 via I2C, shared bus with touch) ---- */
#define BOARD_PIN_IMU_SDA       18  /* shared with touch */
#define BOARD_PIN_IMU_SCL       19  /* shared with touch */
#define BOARD_PIN_IMU_INT1      5
#define BOARD_PIN_IMU_INT2      6
#define BOARD_IMU_I2C_ADDR      0x6B    /* QMI8658 default */

/* ---- SubGHz / CC1101 (no free pins — SD/LCD use GPIO1-4) ---- */
#define BOARD_PIN_CC1101_SCK    UINT16_MAX
#define BOARD_PIN_CC1101_CSN    UINT16_MAX
#define BOARD_PIN_CC1101_MISO   UINT16_MAX
#define BOARD_PIN_CC1101_MOSI   UINT16_MAX
#define BOARD_PIN_CC1101_GDO0   UINT16_MAX

/* ---- Features ---- */
#define BOARD_HAS_TOUCH         1
#define BOARD_HAS_SD_CARD       1
#define BOARD_HAS_BLE           1
#define BOARD_HAS_IMU           1
#define BOARD_HAS_RGB_LED       0
#define BOARD_HAS_VIBRO         0
#define BOARD_HAS_SPEAKER       0
#define BOARD_HAS_IR            0
#define BOARD_HAS_IBUTTON       0
#define BOARD_HAS_RFID          0
#define BOARD_HAS_NFC           0
#define BOARD_HAS_SUBGHZ        0
