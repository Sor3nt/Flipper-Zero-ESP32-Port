#pragma once

#include <stdint.h>

/* ---- Board metadata ---- */
#define BOARD_NAME        "paradoxt"
#define BOARD_ID          "paradoxt"
#define BOARD_TARGET      "esp32s3"
#define BOARD_HAS_BQ25896_SHIP_MODE 1

/* ---- Hardware Button Pins ---- */
#define BOARD_PIN_BUTTON_BOOT   0
#define BOARD_PIN_BUTTON_KEY    21   /* Back, AO3400/QON inverter; active-high with external pull-up */
#define BOARD_PIN_BQ25896_QON   BOARD_PIN_BUTTON_KEY
#define BOARD_PIN_BQ25896_QON_ACTIVE_HIGH 1
#define BOARD_PIN_BTN_UP        41
#define BOARD_PIN_BTN_DOWN      40
#define BOARD_PIN_BTN_LEFT      39
#define BOARD_PIN_BTN_RIGHT     38
/* The driver emits physical Press/Release, then a Short event after Release.
 * OK/Back Long events are inferred by apps from the press duration. */
#define BOARD_INPUT_SHORT_AFTER_RELEASE 1
/* Real D-pad: in the Sub-GHz Transmitter the arrows pick a remote's custom
 * buttons, as on Flipper. Left off on encoder/touch boards (scrolling would
 * transmit) and on Smoochie (arrows are navigation there). */
#define BOARD_SUBGHZ_DPAD_CUSTOM_BUTTONS 1
/* Hardware arrows are a real 4-way D-pad, not the T-Embed's 1-axis encoder.
 * Disables the encoder adaptations in the GUI (ViewInputMode remap in
 * view_dispatcher/view_holder, transposed text_input/button_menu keys,
 * swapped button_panel Left/Right) so every arrow means what it shows. */
#define BOARD_INPUT_DPAD 1
#define BOARD_PIN_BATTERY_ADC   UINT16_MAX
/* No battery divider: voltage/SoC come from the BQ27220 fuel gauge (charger
 * VBAT as fallback). The battery ADC is never initialized on this board. */
#define BOARD_HAS_BATTERY_ADC   0

/* ---- Long-Press Configuration for Shutdown ---- */
#define BOARD_LONGPRESS_SHUTDOWN_MS 2500

/* ---- LCD Pins (ST7789-style panel via SPI) ---- */
#define BOARD_PIN_LCD_MOSI      18
#define BOARD_PIN_LCD_SCLK      17
#define BOARD_PIN_LCD_DC        15
#define BOARD_PIN_LCD_CS        7
#define BOARD_PIN_LCD_RST       16
#define BOARD_PIN_LCD_BL        6

/* ---- LCD Display Configuration (320×170 ST7789) ---- */
#define BOARD_LCD_H_RES         320
#define BOARD_LCD_V_RES         170
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_SPI_FREQ_HZ   (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS      8
#define BOARD_LCD_PARAM_BITS    8
#define BOARD_LCD_SWAP_XY       true
#define BOARD_LCD_MIRROR_X      false
#define BOARD_LCD_MIRROR_Y      true
#define BOARD_LCD_INVERT_COLOR  true
#define BOARD_LCD_GAP_X         0
#define BOARD_LCD_GAP_Y         35
#define BOARD_LCD_BL_ACTIVE_LOW false
#define BOARD_LCD_COLOR_ORDER_BGR false

/* Flipper framebuffer → display color mapping (RGB565, byte-swapped for SPI) */
#define BOARD_LCD_FG_COLOR      0xA0FD
#define BOARD_LCD_FG_COLOR_RB   0x5F03
#define BOARD_LCD_BG_COLOR      0x0000

/* ---- Shared SPI Bus Pins (LCD + SD + CC1101 + NRF24) ---- */
#define BOARD_PIN_SPI_MISO      8
#define BOARD_PIN_SD_CS         3
#define BOARD_PIN_SD_MISO       BOARD_PIN_SPI_MISO
#define BOARD_PIN_EXT_SPI_MOSI  BOARD_PIN_LCD_MOSI
#define BOARD_PIN_EXT_SPI_SCK   BOARD_PIN_LCD_SCLK
#define BOARD_PIN_EXT_SPI_MISO  BOARD_PIN_SPI_MISO

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
#define BOARD_PIN_CC1101_SCK    17
#define BOARD_PIN_CC1101_CSN    9
#define BOARD_PIN_CC1101_MISO   8
#define BOARD_PIN_CC1101_MOSI   18
#define BOARD_PIN_CC1101_GDO0   46
#define BOARD_PIN_CC1101_GDO2   UINT16_MAX
#define BOARD_PIN_CC1101_SW1    UINT16_MAX
#define BOARD_PIN_CC1101_SW0    UINT16_MAX
#define BOARD_CC1101_SPI_SHARED 1
/* Optional external CC1101 module (Sub-GHz > Radio Settings > Module). It shares the
 * radio SPI bus above (MISO 8, MOSI 18, SCK 17); its CSn and GDO0 are two of the
 * five header GPIOs below, chosen by the user and held only while a Sub-GHz session
 * uses the module. No OTG/5 V rail is involved: it is powered from the header. */
#define BOARD_SUBGHZ_EXT_MODULE 1

/* ---- NRF24L01 (shared SPI bus) ---- */
#define BOARD_PIN_NRF24_SCK     BOARD_PIN_CC1101_SCK
#define BOARD_PIN_NRF24_MISO    BOARD_PIN_CC1101_MISO
#define BOARD_PIN_NRF24_MOSI    BOARD_PIN_CC1101_MOSI
#define BOARD_PIN_NRF24_CSN     13
#define BOARD_PIN_NRF24_CE      14
#define BOARD_HAS_NRF24         1
/* Optional external NRF24 module (NRF24 > Radio Settings > Module). Same shared SPI bus;
 * its CSN and CE are two of the five header GPIOs, chosen by the user and held only
 * while an NRF24 scene runs (furi_hal_nrf24_session_begin/end). */
#define BOARD_NRF24_EXT_MODULE  1

/* ---- Infrared (RMT-based) ---- */
#define BOARD_PIN_IR_TX         2
#define BOARD_PIN_IR_RX         1
#define BOARD_PIN_IR_TX_EXT     BOARD_PIN_EXT_GPIO_1 /* Infrared Settings > External. Reserved only while IR transmits; idle = input, pull-down. */

/* ---- Shared external UART (UART1) ----
 * GPIO43/44 are shared by RFID, Expansion modules, and the USB-UART Bridge.
 * UART0 console is disconnected from these pads after HAL initialization so
 * console traffic cannot leak into an attached UART module.
 */
#define BOARD_RFID_UART_NUM     UART_NUM_1
#define BOARD_PIN_RFID_RX       44
#define BOARD_PIN_RFID_TX       43
#define BOARD_HAS_EXPANSION_UART 1
#define BOARD_HAS_USB_UART_BRIDGE 1
#define BOARD_USB_UART_NUM      BOARD_RFID_UART_NUM
#define BOARD_USB_UART_TX       BOARD_PIN_RFID_TX
#define BOARD_USB_UART_RX       BOARD_PIN_RFID_RX
#define BOARD_USB_UART_RTS_GPIO BOARD_PIN_EXT_GPIO_3
#define BOARD_USB_UART_DTR_GPIO BOARD_PIN_EXT_GPIO_4
#define BOARD_USB_UART_DE_RE_GPIO BOARD_PIN_EXT_GPIO_5
#define BOARD_EXPANSION_UART    BOARD_RFID_UART_NUM
#define BOARD_PIN_EXPANSION_TX  BOARD_PIN_RFID_TX
#define BOARD_PIN_EXPANSION_RX  BOARD_PIN_RFID_RX
#define BOARD_EXPANSION_UART_BAUD 9600
#define BOARD_UART0_CONSOLE_DETACH_SHARED_PINS 1

/* ---- Buzzer (GPIO PWM) ---- */
#define BOARD_PIN_BUZZER        10

/* ---- WS2812 RGB LED Strip ---- */
/* WARNING: GPIO45 is a strapping pin (VDD_SPI). Do NOT add strong external pull-up! */
#define BOARD_PIN_WS2812_DATA   45
#define BOARD_WS2812_LED_COUNT  3

/* ---- NFC / PN532 (via I2C) ---- */
#define BOARD_PIN_EXT_I2C_SCL   48
#define BOARD_PIN_EXT_I2C_SDA   47
#define BOARD_EXT_I2C_PORT      I2C_NUM_0
#define BOARD_PIN_NFC_SCL       BOARD_PIN_EXT_I2C_SCL
#define BOARD_PIN_NFC_SDA       BOARD_PIN_EXT_I2C_SDA
#define BOARD_NFC_I2C_PORT      BOARD_EXT_I2C_PORT

/* ---- Qwiic / External I2C (shared with NFC) ---- */
#define BOARD_PIN_QWIIC_SDA     BOARD_PIN_EXT_I2C_SDA
#define BOARD_PIN_QWIIC_SCL     BOARD_PIN_EXT_I2C_SCL

/* ---- Feature Flags ---- */
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

/* ---- Power Management ---- */
#define BQ_I2C_PORT     I2C_NUM_0
#define BQ_I2C_SDA      BOARD_PIN_QWIIC_SDA
#define BQ_I2C_SCL      BOARD_PIN_QWIIC_SCL
#define BQ25896_CHARGE_LIMIT    1280
#define HIGH_DRAIN_CURRENT_THRESHOLD (-200)
#define BQ27220_ADDR            0x55
#define FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH (1500U)

/* ---- External GPIO Pins ---- */
#define BOARD_PIN_EXT_GPIO_1    4
#define BOARD_PIN_EXT_GPIO_2    5
#define BOARD_PIN_EXT_GPIO_3    11
#define BOARD_PIN_EXT_GPIO_4    12
#define BOARD_PIN_EXT_GPIO_5    42

/* GPIO11/12/42 remain ordinary GPIOs except when selected for USB-UART control
 * outputs; USB-UART TX/RX always use GPIO43/44. */

/* Deep-sleep pin states for every remaining usable pad. There is no PWR_EN
 * rail switch on this board, so every peripheral stays powered in deep sleep:
 * a pull must agree with the idle level the peripheral drives, otherwise it
 * leaks through the pull resistor (~73 uA per pin at 3.3 V / 45 kOhm).
 *  - IR receiver output idles high (open collector + internal pull-up).
 *  - Shared MISO is tri-stated by every deselected device; the SD card spec
 *    expects a pull-up on DO.
 *  - GPIO43/44 are UART lines (RFID / expansion / USB-UART): idle high. */
#define FURI_HAL_POWER_EXTRA_PINS \
    {(gpio_num_t)BOARD_PIN_BTN_UP, FuriHalPowerPinPullUp}, \
    {(gpio_num_t)BOARD_PIN_BTN_DOWN, FuriHalPowerPinPullUp}, \
    {(gpio_num_t)BOARD_PIN_BTN_LEFT, FuriHalPowerPinPullUp}, \
    {(gpio_num_t)BOARD_PIN_BTN_RIGHT, FuriHalPowerPinPullUp}, \
    {(gpio_num_t)BOARD_PIN_IR_TX, FuriHalPowerPinLow}, \
    {(gpio_num_t)BOARD_PIN_IR_RX, FuriHalPowerPinPullUp}, \
    {(gpio_num_t)BOARD_PIN_SD_CS, FuriHalPowerPinHigh}, \
    {(gpio_num_t)BOARD_PIN_SPI_MISO, FuriHalPowerPinPullUp}, \
    {(gpio_num_t)BOARD_PIN_CC1101_CSN, FuriHalPowerPinHigh}, \
    {(gpio_num_t)BOARD_PIN_CC1101_GDO0, FuriHalPowerPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_NRF24_CSN, FuriHalPowerPinHigh}, \
    {(gpio_num_t)BOARD_PIN_NRF24_CE, FuriHalPowerPinLow}, \
    {(gpio_num_t)BOARD_PIN_BUZZER, FuriHalPowerPinLow}, \
    {(gpio_num_t)BOARD_PIN_EXT_GPIO_1, FuriHalPowerPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_EXT_GPIO_2, FuriHalPowerPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_EXT_GPIO_3, FuriHalPowerPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_EXT_GPIO_4, FuriHalPowerPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_EXT_GPIO_5, FuriHalPowerPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_RFID_TX, FuriHalPowerPinPullUp}, \
    {(gpio_num_t)BOARD_PIN_RFID_RX, FuriHalPowerPinPullUp},

/* Light-sleep pad states (in addition to the LCD/WS2812/I2C set in
 * furi_hal_power.c). Same idle-level rules as above; outputs that must keep a
 * chip deselected or a transistor off get a matching weak pull. Buttons are
 * wake sources and are configured by the input driver. CC1101 GDO0 is driven
 * low by the radio in SLEEP (IOCFG0 = 0x2F), so the pull-down never fights it. */
#define FURI_HAL_POWER_LIGHT_SLEEP_EXTRA_PINS \
    {(gpio_num_t)BOARD_PIN_IR_TX, FuriHalPowerLsPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_IR_RX, FuriHalPowerLsPinPullUp}, \
    {(gpio_num_t)BOARD_PIN_SD_CS, FuriHalPowerLsPinPullUp}, \
    {(gpio_num_t)BOARD_PIN_SPI_MISO, FuriHalPowerLsPinPullUp}, \
    {(gpio_num_t)BOARD_PIN_CC1101_CSN, FuriHalPowerLsPinPullUp}, \
    {(gpio_num_t)BOARD_PIN_CC1101_GDO0, FuriHalPowerLsPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_NRF24_CSN, FuriHalPowerLsPinPullUp}, \
    {(gpio_num_t)BOARD_PIN_NRF24_CE, FuriHalPowerLsPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_BUZZER, FuriHalPowerLsPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_EXT_GPIO_1, FuriHalPowerLsPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_EXT_GPIO_2, FuriHalPowerLsPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_EXT_GPIO_3, FuriHalPowerLsPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_EXT_GPIO_4, FuriHalPowerLsPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_EXT_GPIO_5, FuriHalPowerLsPinPullDown}, \
    {(gpio_num_t)BOARD_PIN_RFID_TX, FuriHalPowerLsPinPullUp}, \
    {(gpio_num_t)BOARD_PIN_RFID_RX, FuriHalPowerLsPinPullUp},

#if BOARD_PIN_EXPANSION_TX != BOARD_PIN_RFID_TX || \
    BOARD_PIN_EXPANSION_RX != BOARD_PIN_RFID_RX || \
    BOARD_PIN_EXPANSION_TX == BOARD_PIN_EXPANSION_RX || \
    BOARD_PIN_EXPANSION_TX == BOARD_PIN_EXT_GPIO_3 || \
    BOARD_PIN_EXPANSION_TX == BOARD_PIN_EXT_GPIO_4 || \
    BOARD_PIN_EXPANSION_RX == BOARD_PIN_EXT_GPIO_3 || \
    BOARD_PIN_EXPANSION_RX == BOARD_PIN_EXT_GPIO_4
#error "Paradoxt Expansion, RFID, and USB-UART must share distinct GPIO43/44 pins"
#endif

#define BOARD_SHUTDOWN_BUTTON   BOARD_PIN_BUTTON_KEY
#define BOARD_SHUTDOWN_TIME_MS  BOARD_LONGPRESS_SHUTDOWN_MS
#define BOARD_SHUTDOWN_BUTTON_ACTIVE_HIGH 1
