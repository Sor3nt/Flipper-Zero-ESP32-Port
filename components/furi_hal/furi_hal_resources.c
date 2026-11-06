/**
 * @file furi_hal_resources.c
 * Resources HAL — pin definitions driven by board header
 */

#include "furi_hal_resources.h"
#include "boards/board.h"

const InputPin input_pins[] = {};
const size_t input_pins_count = 0;

#if defined(BOARD_PIN_EXT_GPIO_1)
#if !defined(BOARD_PIN_EXT_GPIO_2) || !defined(BOARD_PIN_EXT_GPIO_3) || \
    !defined(BOARD_PIN_EXT_GPIO_4) || !defined(BOARD_PIN_EXT_GPIO_5)
#error "An external GPIO header needs BOARD_PIN_EXT_GPIO_1..5"
#endif
#define FURI_HAL_STRINGIFY_INNER(value) #value
#define FURI_HAL_STRINGIFY(value)       FURI_HAL_STRINGIFY_INNER(value)
static const GpioPin gpio_header_ext_1 = {.port = NULL, .pin = BOARD_PIN_EXT_GPIO_1};
static const GpioPin gpio_header_ext_2 = {.port = NULL, .pin = BOARD_PIN_EXT_GPIO_2};
static const GpioPin gpio_header_ext_3 = {.port = NULL, .pin = BOARD_PIN_EXT_GPIO_3};
static const GpioPin gpio_header_ext_4 = {.port = NULL, .pin = BOARD_PIN_EXT_GPIO_4};
static const GpioPin gpio_header_ext_5 = {.port = NULL, .pin = BOARD_PIN_EXT_GPIO_5};
const GpioPinRecord gpio_pins[] = {
    {.pin = &gpio_header_ext_1, .name = "GPIO" FURI_HAL_STRINGIFY(BOARD_PIN_EXT_GPIO_1), .debug = false},
    {.pin = &gpio_header_ext_2, .name = "GPIO" FURI_HAL_STRINGIFY(BOARD_PIN_EXT_GPIO_2), .debug = false},
    {.pin = &gpio_header_ext_3, .name = "GPIO" FURI_HAL_STRINGIFY(BOARD_PIN_EXT_GPIO_3), .debug = false},
    {.pin = &gpio_header_ext_4, .name = "GPIO" FURI_HAL_STRINGIFY(BOARD_PIN_EXT_GPIO_4), .debug = false},
    {.pin = &gpio_header_ext_5, .name = "GPIO" FURI_HAL_STRINGIFY(BOARD_PIN_EXT_GPIO_5), .debug = false},
};
const size_t gpio_pins_count = sizeof(gpio_pins) / sizeof(gpio_pins[0]);
#endif

/* Hardware Button Pins */
const GpioPin gpio_button_boot  = {.port = NULL, .pin = BOARD_PIN_BUTTON_BOOT};
const GpioPin gpio_battery_sense = {.port = NULL, .pin = BOARD_PIN_BATTERY_ADC};

/* LCD Pins */
const GpioPin gpio_lcd_din = {.port = NULL, .pin = BOARD_PIN_LCD_MOSI};
const GpioPin gpio_lcd_clk = {.port = NULL, .pin = BOARD_PIN_LCD_SCLK};
const GpioPin gpio_lcd_dc  = {.port = NULL, .pin = BOARD_PIN_LCD_DC};
const GpioPin gpio_lcd_cs  = {.port = NULL, .pin = BOARD_PIN_LCD_CS};
const GpioPin gpio_lcd_rst = {.port = NULL, .pin = BOARD_PIN_LCD_RST};
const GpioPin gpio_lcd_bl  = {.port = NULL, .pin = BOARD_PIN_LCD_BL};

#ifdef BOARD_SD_BUS_NOT_SHARED
const GpioPin gpio_sdcard_miso2 = {.port = NULL, .pin = BOARD_PIN_SD_MISO2};
const GpioPin gpio_sdcard_mosi  = {.port = NULL, .pin = BOARD_PIN_SD_MOSI};
const GpioPin gpio_sdcard_clk   = {.port = NULL, .pin = BOARD_PIN_SD_SCK};
#endif

/* SD Card Pins */
const GpioPin gpio_sdcard_cs   = {.port = NULL, .pin = BOARD_PIN_SD_CS};
const GpioPin gpio_sdcard_miso = {.port = NULL, .pin = BOARD_PIN_SD_MISO};

/* Touch Pins */
const GpioPin gpio_touch_scl = {.port = NULL, .pin = BOARD_PIN_TOUCH_SCL};
const GpioPin gpio_touch_sda = {.port = NULL, .pin = BOARD_PIN_TOUCH_SDA};
const GpioPin gpio_touch_rst = {.port = NULL, .pin = BOARD_PIN_TOUCH_RST};
const GpioPin gpio_touch_int = {.port = NULL, .pin = BOARD_PIN_TOUCH_INT};

/* External / CC1101 pins */
const GpioPin gpio_ext_pc0  = {.port = NULL, .pin = UINT16_MAX};
const GpioPin gpio_ext_pc1  = {.port = NULL, .pin = UINT16_MAX};
const GpioPin gpio_ext_pc3  = {.port = NULL, .pin = UINT16_MAX};
const GpioPin gpio_ext_pb2  = {.port = NULL, .pin = UINT16_MAX};
const GpioPin gpio_ext_pb3  = {.port = NULL, .pin = BOARD_PIN_CC1101_SCK};
const GpioPin gpio_ext_pa4  = {.port = NULL, .pin = BOARD_PIN_CC1101_CSN};
const GpioPin gpio_ext_pa6  = {.port = NULL, .pin = BOARD_PIN_CC1101_MISO};
const GpioPin gpio_ext_pa7  = {.port = NULL, .pin = BOARD_PIN_CC1101_MOSI};
const GpioPin gpio_cc1101_g0 = {.port = NULL, .pin = BOARD_PIN_CC1101_GDO0};

#ifdef BOARD_PIN_NRF24_CSN
const GpioPin gpio_nrf24_cs = {.port = NULL, .pin = BOARD_PIN_NRF24_CSN};
#else
const GpioPin gpio_nrf24_cs = {.port = NULL, .pin = UINT16_MAX};
#endif
const GpioPin gpio_ibutton  = {.port = NULL, .pin = UINT16_MAX};
const GpioPin gpio_speaker  = {.port = NULL, .pin = UINT16_MAX};
