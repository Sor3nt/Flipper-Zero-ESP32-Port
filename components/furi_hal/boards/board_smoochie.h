/**
 * @file board_smoochie.h
 * Board definition: smoochie (ESP32-S3 N16R8)
 *
 * MCU:      ESP32-S3 N16R8 (16 MB quad flash + 8 MB OCTAL PSRAM)
 *           -> GPIO26..32 (flash) and GPIO35/36/37 (octal PSRAM) are NOT usable.
 * Display:  ST7789 1.9" 170x320 panel via SPI, used in landscape (320x170)
 * Input:    5-button pad (Up/Down/Left/Right/Select)
 * SubGHz:   CC1101 + NRF24L01 on shared SPI bus
 * IR:       RX/TX
 * SD Card:  SPI
 * Power:    BQ25896 charger / battery monitor
 *
 * Pin-safety notes (ESP32-S3):
 *   strapping pins : 0, 3, 45, 46
 *   USB D-/D+      : 19, 20 (not used here)
 *   RFID UART RX/TX: GPIO44 / GPIO43
 *   ADC-capable    : ADC1 = GPIO1..10, ADC2 = GPIO11..20  (GPIO42 has NO ADC)
 */

#pragma once

#include <stdint.h>

/* ---- Board metadata ---- */
#define BOARD_NAME        "smoochie"
#define BOARD_ID          "smoochie"
#define BOARD_TARGET      "esp32s3"
#define BOARD_HAS_BQ25896_SHIP_MODE 1
#define BOARD_HAS_BQ27220 0

/* ---- Hardware Button Pins ----
 * Up/Left/Right/Select are active low with internal pull-ups.
 * Down/GPIO40 is active high through the AO3400/QON inverter and uses an
 * external pull-up.
 */
#define BOARD_PIN_BUTTON_BOOT   0    /* Select/OK. Strapping pin: do not hold low at reset */
#define BOARD_PIN_BTN_UP        41   /* tap = Up, hold 2.5 s = shutdown */
#define BOARD_PIN_BTN_DOWN      40   /* tap = Down, hold 0.8 s = Back */
#define BOARD_PIN_BTN_LEFT      39
#define BOARD_PIN_BTN_RIGHT     38

/* Calibrated from VBAT=4.15V and GPIO2=1.35V on the assembled board.
 * With a 241k lower resistor, the measured ratio implies about 500k on top.
 * The same-point firmware reading (1540mV at GPIO2) needs this ADC correction. */
#define BOARD_BATTERY_ADC_PRIMARY 1
#define BOARD_PIN_BATTERY_ADC   2
#define BOARD_BATTERY_DIVIDER_TOP_OHM 499000UL
#define BOARD_BATTERY_DIVIDER_BOTTOM_OHM 241000UL
#define BOARD_BATTERY_ADC_CORRECTION_NUMERATOR   1000UL
#define BOARD_BATTERY_ADC_CORRECTION_DENOMINATOR 1000UL
#define BOARD_BATTERY_ADC_FILTER_CAP_NF 100UL

/* ---- Long-Press Configuration for Shutdown / Reset ---- */
#define BOARD_LONGPRESS_SHUTDOWN_MS 2500
#define BOARD_SHUTDOWN_BUTTON       BOARD_PIN_BTN_UP
#define BOARD_SHUTDOWN_TIME_MS      BOARD_LONGPRESS_SHUTDOWN_MS
/* /QON is a charger-side net.  GPIO40 reads it through the AO3400
 * isolator and must not be used as a direct /QON level during shutdown:
 * GPIO40 is LOW when /QON is HIGH in the released state. */
/* GPIO40 is the AO3400 drain, so it is HIGH only while /QON is asserted. */
#define BOARD_PIN_BQ25896_QON_ACTIVE_HIGH 1

/* ---- LCD Pins (ST7789 1.9" via SPI) ---- */
#define BOARD_PIN_LCD_MOSI      17
#define BOARD_PIN_LCD_SCLK      18
#define BOARD_PIN_LCD_DC        15
#define BOARD_PIN_LCD_CS        7
#define BOARD_PIN_LCD_RST       16
#define BOARD_PIN_LCD_BL        6

/* ---- LCD Display Configuration ----
 * H_RES / V_RES are the LOGICAL size AFTER swap_xy (landscape): 320 x 170.
 * The panel itself is 170x320 (portrait). With BOARD_LCD_SWAP_XY = true the driver
 * must be told 320x170; declaring 170x320 made only ~half of the screen get drawn.
 * GAP_Y = 35 is the usual 240x320-RAM offset for the 170-pixel-wide 1.9" panel.
 * If the picture appears rotated 180 deg / mirrored, flip BOTH mirror flags
 * (true,false) <-> (false,true); paradoxt uses (false,true). */
#define BOARD_LCD_H_RES         320
#define BOARD_LCD_V_RES         170
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_SPI_FREQ_HZ   (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS      8
#define BOARD_LCD_PARAM_BITS    8
#define BOARD_LCD_SWAP_XY       true
#define BOARD_LCD_MIRROR_X      true
#define BOARD_LCD_MIRROR_Y      false
#define BOARD_LCD_INVERT_COLOR  true
#define BOARD_LCD_GAP_X         0
#define BOARD_LCD_GAP_Y         35
#define BOARD_LCD_BL_ACTIVE_LOW false
#define BOARD_LCD_COLOR_ORDER_BGR false

/* Flipper framebuffer → display color mapping (RGB565, byte-swapped for SPI) */
#define BOARD_LCD_FG_COLOR      0xA0FD
#define BOARD_LCD_FG_COLOR_RB   0x5F03
#define BOARD_LCD_BG_COLOR      0x0000

/* ---- LCD + SD SPI2 bus pins ---- */
#define BOARD_PIN_SD_CS         3    /* strapping pin (JTAG source) - fine as CS, same as paradoxt */
#define BOARD_PIN_SD_MISO       8

/* ---- Dedicated radio SPI3 bus (CC1101 + NRF24) ----
 * The Smoochie schematic routes the radio bus separately from LCD/SD:
 * MOSI=GPIO12, MISO=GPIO11, SCK=GPIO13. */
#define BOARD_CC1101_SPI_HOST   SPI3_HOST
#define BOARD_PIN_RADIO_SPI_MOSI 12
#define BOARD_PIN_RADIO_SPI_MISO 11
#define BOARD_PIN_RADIO_SPI_SCK  13
#define BOARD_PIN_CC1101_SCK    BOARD_PIN_RADIO_SPI_SCK
#define BOARD_PIN_CC1101_CSN    9
#define BOARD_PIN_CC1101_MISO   BOARD_PIN_RADIO_SPI_MISO
#define BOARD_PIN_CC1101_MOSI   BOARD_PIN_RADIO_SPI_MOSI
#define BOARD_PIN_CC1101_GDO0   46
#define BOARD_PIN_CC1101_GDO2   UINT16_MAX
#define BOARD_PIN_CC1101_SW1    UINT16_MAX
#define BOARD_PIN_CC1101_SW0    UINT16_MAX
#define BOARD_CC1101_SPI_SHARED 0

/* Keep powered peripherals electrically quiet during ESP32 deep sleep. The
 * macro is consumed by furi_hal_power.c after its pin-mode enum is defined. */
#define FURI_HAL_POWER_EXTRA_PINS \
    /* Button inputs: preserve external bias on Down, bias the active-low keys. */ \
    {(gpio_num_t)BOARD_PIN_BTN_DOWN, FuriHalPowerPinExternal}, \
    {(gpio_num_t)BOARD_PIN_BTN_LEFT, FuriHalPowerPinPullUp}, \
    {(gpio_num_t)BOARD_PIN_BTN_RIGHT, FuriHalPowerPinPullUp}, \
    /* Shared SPI2: deselect SD and prevent its MISO from floating. */ \
    {(gpio_num_t)BOARD_PIN_SD_CS, FuriHalPowerPinHigh}, \
    {(gpio_num_t)BOARD_PIN_SD_MISO, FuriHalPowerPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_CC1101_CSN, FuriHalPowerPinHigh}, \
    {(gpio_num_t)BOARD_PIN_RADIO_SPI_MOSI, FuriHalPowerPinLow}, \
    {(gpio_num_t)BOARD_PIN_RADIO_SPI_MISO, FuriHalPowerPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_RADIO_SPI_SCK, FuriHalPowerPinLow}, \
    {(gpio_num_t)BOARD_PIN_NRF24_CSN, FuriHalPowerPinHigh}, \
    {(gpio_num_t)BOARD_PIN_NRF24_CE, FuriHalPowerPinLow}, \
    {(gpio_num_t)BOARD_PIN_CC1101_GDO0, FuriHalPowerPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_IR_TX, FuriHalPowerPinLow}, \
    {(gpio_num_t)BOARD_PIN_IR_RX, FuriHalPowerPinPullDown}, \
    /* Board-local outputs and disconnected digital pads. */ \
    {(gpio_num_t)BOARD_PIN_BUZZER, FuriHalPowerPinLow}, \
    {(gpio_num_t)10, FuriHalPowerPinPullDown}, \
    {(gpio_num_t)42, FuriHalPowerPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_RFID_TX, FuriHalPowerPinLow}, \
    {(gpio_num_t)BOARD_PIN_RFID_RX, FuriHalPowerPinPullDown},

/* ---- NRF24L01 (shared dedicated radio SPI3 bus) ---- */
#define BOARD_PIN_NRF24_SCK     BOARD_PIN_RADIO_SPI_SCK
#define BOARD_PIN_NRF24_MISO    BOARD_PIN_RADIO_SPI_MISO
#define BOARD_PIN_NRF24_MOSI    BOARD_PIN_RADIO_SPI_MOSI
#define BOARD_PIN_NRF24_CSN     14
#define BOARD_PIN_NRF24_CE      21
#define BOARD_HAS_NRF24         1

#if BOARD_PIN_CC1101_CSN == BOARD_PIN_NRF24_CSN || \
    BOARD_PIN_CC1101_CSN == BOARD_PIN_LCD_CS || \
    BOARD_PIN_CC1101_CSN == BOARD_PIN_SD_CS || \
    BOARD_PIN_NRF24_CSN == BOARD_PIN_LCD_CS || \
    BOARD_PIN_NRF24_CSN == BOARD_PIN_SD_CS || \
    BOARD_PIN_LCD_CS == BOARD_PIN_SD_CS
#error "Smoochie SPI chip-select pins must be distinct"
#endif

#if BOARD_PIN_CC1101_CSN == BOARD_PIN_RADIO_SPI_MOSI || \
    BOARD_PIN_CC1101_CSN == BOARD_PIN_RADIO_SPI_MISO || \
    BOARD_PIN_CC1101_CSN == BOARD_PIN_RADIO_SPI_SCK || \
    BOARD_PIN_NRF24_CSN == BOARD_PIN_RADIO_SPI_MOSI || \
    BOARD_PIN_NRF24_CSN == BOARD_PIN_RADIO_SPI_MISO || \
    BOARD_PIN_NRF24_CSN == BOARD_PIN_RADIO_SPI_SCK
#error "Smoochie radio CSN pin overlaps a shared SPI signal"
#endif

#if BOARD_PIN_RADIO_SPI_MOSI == BOARD_PIN_LCD_MOSI || \
    BOARD_PIN_RADIO_SPI_MOSI == BOARD_PIN_LCD_SCLK || \
    BOARD_PIN_RADIO_SPI_MOSI == BOARD_PIN_LCD_CS || \
    BOARD_PIN_RADIO_SPI_MISO == BOARD_PIN_LCD_MOSI || \
    BOARD_PIN_RADIO_SPI_MISO == BOARD_PIN_LCD_SCLK || \
    BOARD_PIN_RADIO_SPI_MISO == BOARD_PIN_LCD_CS || \
    BOARD_PIN_RADIO_SPI_SCK == BOARD_PIN_LCD_MOSI || \
    BOARD_PIN_RADIO_SPI_SCK == BOARD_PIN_LCD_SCLK || \
    BOARD_PIN_RADIO_SPI_SCK == BOARD_PIN_LCD_CS || \
    BOARD_PIN_RADIO_SPI_MISO == BOARD_PIN_SD_MISO || \
    BOARD_PIN_RADIO_SPI_MOSI == BOARD_PIN_SD_MISO || \
    BOARD_PIN_RADIO_SPI_SCK == BOARD_PIN_SD_MISO || \
    BOARD_PIN_RADIO_SPI_MISO == BOARD_PIN_SD_CS || \
    BOARD_PIN_RADIO_SPI_MOSI == BOARD_PIN_SD_CS || \
    BOARD_PIN_RADIO_SPI_SCK == BOARD_PIN_SD_CS
#error "Smoochie radio SPI pins overlap LCD or SD pins"
#endif

#if BOARD_PIN_SD_MISO == BOARD_PIN_LCD_MOSI || \
    BOARD_PIN_SD_MISO == BOARD_PIN_LCD_SCLK || \
    BOARD_PIN_SD_MISO == BOARD_PIN_LCD_CS || \
    BOARD_PIN_SD_MISO == BOARD_PIN_SD_CS
#error "Smoochie SD signal pins overlap LCD or SD chip-select pins"
#endif

/* ---- Infrared (RMT-based) ---- */
#define BOARD_PIN_IR_TX         5
#define BOARD_PIN_IR_RX         4

/* ---- Shared external UART (UART1) ----
 * GPIO43/44 are shared by the removable RFID reader, Expansion modules, and
 * USB-UART Bridge. Their applications must serialize ownership of this port.
 * UART0 console output is rerouted while one of them owns these pins. */
#define BOARD_RFID_UART_NUM     1
#define BOARD_PIN_RFID_RX       44
#define BOARD_PIN_RFID_TX       43
#define BOARD_HAS_EXPANSION_UART 1
#define BOARD_HAS_USB_UART_BRIDGE 1
#define BOARD_USB_UART_NUM      1
#define BOARD_USB_UART_TX       BOARD_PIN_RFID_TX
#define BOARD_USB_UART_RX       BOARD_PIN_RFID_RX
#define BOARD_UART0_CONSOLE_TX  43
#define BOARD_UART0_CONSOLE_RX  44
/* GPIO10 and GPIO42 are unconnected on this board; route console UART away
 * from the shared RFID/USB-UART pins while their UART resource is owned. */
#define BOARD_UART0_SINK_TX     42
#define BOARD_UART0_SINK_RX     10

#if BOARD_UART0_SINK_TX == BOARD_PIN_RFID_TX || \
    BOARD_UART0_SINK_TX == BOARD_PIN_RFID_RX || \
    BOARD_UART0_SINK_RX == BOARD_PIN_RFID_TX || \
    BOARD_UART0_SINK_RX == BOARD_PIN_RFID_RX || \
    BOARD_UART0_SINK_TX == BOARD_UART0_SINK_RX
#error "UART0 sink pins must not overlap the shared UART or each other"
#endif

/* ---- Buzzer (GPIO PWM) ----
 * GPIO1 is a digital-capable output and is no longer used by RFID UART. */
#define BOARD_PIN_BUZZER        1

/* ---- WS2812 RGB LED Strip ----
 * GPIO45 is a strapping pin (VDD_SPI voltage). Do NOT add a strong external pull-up. */
#define BOARD_PIN_WS2812_DATA   45
#define BOARD_WS2812_LED_COUNT  16

/* ---- NFC / PN532 (via I2C) ---- */
#define BOARD_PIN_NFC_SCL       48
#define BOARD_PIN_NFC_SDA       47
#define BOARD_NFC_I2C_PORT      I2C_NUM_0

/* ---- Ebyte E07-400M20S RF front-end control via AW9523 ----
 * AW9523 P0_7 (IO_EXP_CC_RX) = RXEN, P1_4 (IO_EXP_CC_TX) = TXEN.
 * The expander uses the same I2C bus as
 * the charger and fuel gauge; its default 7-bit address is 0x58. */
#define BOARD_HAS_AW9523       1
#define BOARD_AW9523_I2C_ADDR  0x58
#define BOARD_AW9523_RXEN_P0_7 1
#define BOARD_AW9523_TXEN_P1_4 1

/* ---- Qwiic / External I2C (shared with NFC) ---- */
#define BOARD_PIN_QWIIC_SDA     47
#define BOARD_PIN_QWIIC_SCL     48

/* ---- Touch Controller — NOT PRESENT ---- */
#define BOARD_PIN_TOUCH_SCL     UINT16_MAX
#define BOARD_PIN_TOUCH_SDA     UINT16_MAX
#define BOARD_PIN_TOUCH_RST     UINT16_MAX
#define BOARD_PIN_TOUCH_INT     UINT16_MAX
#define BOARD_TOUCH_I2C_ADDR    0x00
#define BOARD_TOUCH_I2C_PORT    I2C_NUM_0
#define BOARD_TOUCH_I2C_FREQ_HZ 0
#define BOARD_TOUCH_I2C_TIMEOUT 0

/* ---- Features ---- */
#define BOARD_HAS_TOUCH         0
#define BOARD_HAS_SD_CARD       1
#define BOARD_HAS_BLE           1
#define BOARD_HAS_RGB_LED       1
#define BOARD_HAS_VIBRO         0
#define BOARD_HAS_SPEAKER       0
#define BOARD_HAS_BUZZER        1
#define BOARD_HAS_IR            1
#define BOARD_HAS_IBUTTON       0
#define BOARD_HAS_RFID          1
#define BOARD_HAS_NFC           1
#define BOARD_HAS_SUBGHZ        1
#define BOARD_HAS_MIC           0

/* ---- Power management / charger ---- */
#define BQ_I2C_PORT     I2C_NUM_0
#define BQ_I2C_SDA      BOARD_PIN_QWIIC_SDA
#define BQ_I2C_SCL      BOARD_PIN_QWIIC_SCL
#define BQ25896_CHARGE_LIMIT    1280
#define HIGH_DRAIN_CURRENT_THRESHOLD (-200)
#define BQ27220_ADDR            0x55
#define FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH (1500U)

/* No expansion UART is routed to external GPIO pins on this board. */
