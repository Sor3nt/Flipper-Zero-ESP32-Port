/**
 * @file furi_hal_resources.c
 * Resources HAL — pin definitions driven by board header
 */

#include "furi_hal_resources.h"
#include "boards/board.h"

const InputPin input_pins[] = {};
const size_t input_pins_count = 0;

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
const GpioPin gpio_ibutton  = {.port = NULL, .pin = UINT16_MAX};
const GpioPin gpio_speaker  = {.port = NULL, .pin = UINT16_MAX};
