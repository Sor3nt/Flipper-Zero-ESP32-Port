/**
 * @file board_paradoxt.h
 * Board definition: Paradoxt
 *
 * MCU:      ESP32-S3 N16R8 (dual-core, see sdkconfig.defaults.paradoxt)
 * Display:  ST7789 320x170 RGB565 via SPI
 * Input:    6 buttons: Up/Down/Left/Right D-pad, OK (BOOT), Back (BQ25896 /QON)
 * SubGHz:   CC1101 via SPI (shared bus with LCD + SD + NRF24)
 * NRF24:    NRF24L01 via SPI (shared bus)
 * NFC:      PN532 via I2C (shared with BQ25896 / BQ27220 / Qwiic)
 * Power:    BQ25896 charger (BATFET ship-mode power-off), BQ27220 fuel gauge
 * IR:       TX (IO02) + RX (IO01), optional external TX on header GPIO4
 * RGB LED:  WS2812 x3 (IO45)
 * Buzzer:   passive, LEDC PWM (IO10)
 * Header:   5 external GPIOs (IO4, IO5, IO11, IO12, IO42) + UART1 (IO43/IO44)
 */

#pragma once

#include <stdint.h>

/* ---- Board metadata ---- */
#define BOARD_NAME   "Paradoxt"
#define BOARD_ID     "paradoxt"
/* The ported apps (FAPs) are supported on this board; see furi_hal_board_supports_ported_apps(). */
#define BOARD_PORTED_APPS_SUPPORTED 1
#define BOARD_TARGET "esp32s3"

/* Paradoxt-only power behaviour: BATFET ship-mode power-off, charger sleep
 * preparation, /QON-aware shutdown. Every user is guarded by this macro. */
#define BOARD_HAS_PARADOXT_POWER_SEQUENCE 1
#define BOARD_HAS_BQ25896_SHIP_MODE       1

/* ---- Hardware Buttons ---- */
#define BOARD_PIN_BUTTON_BOOT 0  /* OK (BOOT/IO00, active low, external pull-up) */
#define BOARD_PIN_BUTTON_KEY  21 /* Back: AO3400 inverter on BQ25896 /QON, active high, external pull-up */
#define BOARD_PIN_BQ25896_QON BOARD_PIN_BUTTON_KEY
#define BOARD_PIN_BQ25896_QON_ACTIVE_HIGH 1
#define BOARD_PIN_BTN_UP    41
#define BOARD_PIN_BTN_DOWN  40
#define BOARD_PIN_BTN_LEFT  39
#define BOARD_PIN_BTN_RIGHT 38
#define BOARD_PIN_BTN_OK    BOARD_PIN_BUTTON_BOOT
#define BOARD_PIN_BTN_BACK  BOARD_PIN_BUTTON_KEY
/* Forced reboot combo (CONFIG_FURI_HAL_REBOOT_COMBO): hold Up + Down for 5 s. */
#define BOARD_REBOOT_COMBO_PIN_A BOARD_PIN_BTN_UP
#define BOARD_REBOOT_COMBO_PIN_B BOARD_PIN_BTN_DOWN
/* Real 4-way D-pad, not the T-Embed's 1-axis encoder: disables the encoder
 * adaptations in the GUI so every arrow means what it shows. */
#define BOARD_INPUT_DPAD 1
#define BOARD_PIN_BATTERY_ADC UINT16_MAX /* Voltage / SoC come from the BQ27220 */

/* ---- LCD Pins (ST7789 via SPI) ---- */
#define BOARD_PIN_LCD_MOSI 18
#define BOARD_PIN_LCD_SCLK 17
#define BOARD_PIN_LCD_DC   15
#define BOARD_PIN_LCD_CS   7
#define BOARD_PIN_LCD_RST  16
#define BOARD_PIN_LCD_BL   6 /* Backlight PWM, active high */

/* ---- LCD Display Configuration ---- */
#define BOARD_LCD_H_RES           320
#define BOARD_LCD_V_RES           170
#define BOARD_LCD_SPI_HOST        SPI2_HOST
#define BOARD_LCD_SPI_FREQ_HZ     (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS        8
#define BOARD_LCD_PARAM_BITS      8
#define BOARD_LCD_SWAP_XY         true
#define BOARD_LCD_MIRROR_X        false
#define BOARD_LCD_MIRROR_Y        true
#define BOARD_LCD_INVERT_COLOR    true
#define BOARD_LCD_GAP_X           0
#define BOARD_LCD_GAP_Y           35
#define BOARD_LCD_BL_ACTIVE_LOW   false
#define BOARD_LCD_COLOR_ORDER_BGR false

/* Flipper framebuffer → display color mapping (RGB565, byte-swapped for SPI) */
#define BOARD_LCD_FG_COLOR    0xA0FD
#define BOARD_LCD_FG_COLOR_RB 0x5F03
#define BOARD_LCD_BG_COLOR    0x0000

/* ---- SD Card (shared SPI bus with LCD, CC1101 and NRF24) ---- */
#define BOARD_PIN_SD_CS   3 /* Strapping pin (JTAG source); idles high */
#define BOARD_PIN_SD_MISO 8

/* ---- Touch Controller — NOT PRESENT ---- */
#define BOARD_PIN_TOUCH_SCL     UINT16_MAX
#define BOARD_PIN_TOUCH_SDA     UINT16_MAX
#define BOARD_PIN_TOUCH_RST     UINT16_MAX
#define BOARD_PIN_TOUCH_INT     UINT16_MAX
#define BOARD_TOUCH_I2C_ADDR    0x00
#define BOARD_TOUCH_I2C_PORT    I2C_NUM_0
#define BOARD_TOUCH_I2C_FREQ_HZ 0
#define BOARD_TOUCH_I2C_TIMEOUT 0

/* ---- SubGHz / CC1101 (shared SPI bus) ---- */
#define BOARD_PIN_CC1101_SCK    17
#define BOARD_PIN_CC1101_CSN    9
#define BOARD_PIN_CC1101_MISO   8
#define BOARD_PIN_CC1101_MOSI   18
#define BOARD_PIN_CC1101_GDO0   46
#define BOARD_PIN_CC1101_GDO2   UINT16_MAX
#define BOARD_PIN_CC1101_SW1    UINT16_MAX
#define BOARD_PIN_CC1101_SW0    UINT16_MAX
#define BOARD_CC1101_SPI_SHARED 1

/* ---- NRF24L01 (shared SPI bus) ---- */
#define BOARD_PIN_NRF24_SCK  BOARD_PIN_CC1101_SCK
#define BOARD_PIN_NRF24_MISO BOARD_PIN_CC1101_MISO
#define BOARD_PIN_NRF24_MOSI BOARD_PIN_CC1101_MOSI
#define BOARD_PIN_NRF24_CSN  13
#define BOARD_PIN_NRF24_CE   14
#define BOARD_HAS_NRF24      1

/* ---- IR ---- */
#define BOARD_PIN_IR_TX 2
#define BOARD_PIN_IR_RX 1

/* ---- External header GPIOs ---- */
#define BOARD_PIN_EXT_GPIO_1 4
#define BOARD_PIN_EXT_GPIO_2 5
#define BOARD_PIN_EXT_GPIO_3 11
#define BOARD_PIN_EXT_GPIO_4 12
#define BOARD_PIN_EXT_GPIO_5 42
/* Infrared Settings > TX Pin: External. Claimed only while IR transmits. */
#define BOARD_PIN_IR_TX_EXT BOARD_PIN_EXT_GPIO_1
/* Free header pads for the I2C sniffer (SDA / SCL). Not EXT_GPIO_1: that is the external
 * IR output, and the sniffer and an IR transmission must never want the same pad. The
 * sniffer leases both pads with furi_hal_gpio_reserve(). */
#define BOARD_PIN_I2C_SNIFF_SDA BOARD_PIN_EXT_GPIO_2
#define BOARD_PIN_I2C_SNIFF_SCL BOARD_PIN_EXT_GPIO_3

/* ---- NFC / PN532 (via I2C, shared with the power ICs) ---- */
#define BOARD_PIN_NFC_SCL  48
#define BOARD_PIN_NFC_SDA  47
#define BOARD_NFC_I2C_PORT I2C_NUM_0

/* ---- Speaker — NOT PRESENT (passive buzzer instead) ---- */
#define BOARD_PIN_SPEAKER_BCLK UINT16_MAX
#define BOARD_PIN_SPEAKER_WCLK UINT16_MAX
#define BOARD_PIN_SPEAKER_DOUT UINT16_MAX

/* ---- Buzzer (LEDC PWM) ---- */
#define BOARD_PIN_BUZZER 10

/* ---- WS2812 RGB LED Strip ---- */
/* GPIO45 is a strapping pin (VDD_SPI): never add a strong external pull-up. */
#define BOARD_PIN_WS2812_DATA  45
#define BOARD_WS2812_LED_COUNT 3

/* ---- Qwiic / External I2C (shared with NFC and the power ICs) ---- */
#define BOARD_PIN_QWIIC_SDA 47
#define BOARD_PIN_QWIIC_SCL 48

/* ---- Shared external UART (UART1 on GPIO43/44) ----
 * RFID, the USB-UART bridge and expansion modules take turns on these pads.
 * The UART0 console is detached from them after HAL init. */
#define BOARD_RFID_UART_NUM 1
#define BOARD_PIN_RFID_RX   44
#define BOARD_PIN_RFID_TX   43
#define BOARD_UART0_CONSOLE_DETACH_SHARED_PINS 1
#define BOARD_HAS_USB_UART_BRIDGE 1
#define BOARD_USB_UART_NUM        BOARD_RFID_UART_NUM
#define BOARD_USB_UART_TX         BOARD_PIN_RFID_TX
#define BOARD_USB_UART_RX         BOARD_PIN_RFID_RX
#define BOARD_USB_UART_RTS_GPIO   BOARD_PIN_EXT_GPIO_3
#define BOARD_USB_UART_DTR_GPIO   BOARD_PIN_EXT_GPIO_4
#define BOARD_USB_UART_DE_RE_GPIO BOARD_PIN_EXT_GPIO_5

/* ---- Features ---- */
#define BOARD_HAS_TOUCH   0
#define BOARD_HAS_ENCODER 0
#define BOARD_HAS_SD_CARD 1
#define BOARD_HAS_BLE     1
#define BOARD_HAS_RGB_LED 1
#define BOARD_HAS_VIBRO   0
#define BOARD_HAS_SPEAKER 0
#define BOARD_HAS_BUZZER  1
#define BOARD_HAS_IR      1
#define BOARD_HAS_IBUTTON 0
#define BOARD_HAS_RFID    1
#define BOARD_HAS_NFC     1
#define BOARD_HAS_SUBGHZ  1
#define BOARD_HAS_MIC     0

/* ---- Power Management ---- */
#define BQ27220_ADDR 0x55
#define BQ_I2C_PORT  I2C_NUM_0
#define BQ_I2C_SDA   BOARD_PIN_QWIIC_SDA
#define BQ_I2C_SCL   BOARD_PIN_QWIIC_SCL
#define HIGH_DRAIN_CURRENT_THRESHOLD        (-200)
#define FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH (1500U)
#define BQ25896_CHARGE_LIMIT                1280
