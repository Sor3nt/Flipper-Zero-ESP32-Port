/**
 * @file furi_hal_resources.h
 * Resources HAL API (ESP32 stub)
 */

#pragma once

#include <stddef.h>
#include <furi_hal_gpio.h>
#include <input.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Input Related Constants */
#define INPUT_DEBOUNCE_TICKS 4

/* Light */
typedef enum {
    LightRed = (1 << 0),
    LightGreen = (1 << 1),
    LightBlue = (1 << 2),
    LightBacklight = (1 << 3),
} Light;

typedef struct {
    const GpioPin* gpio;
    const InputKey key;
    const bool inverted;
    const char* name;
} InputPin;

extern const InputPin input_pins[];
extern const size_t input_pins_count;

/* Hardware Button Pins (Waveshare ESP32-C6-Touch-LCD-1.9) */
extern const GpioPin gpio_button_boot; /* GPIO9 - BOOT button (active low) */
extern const GpioPin gpio_battery_sense; /* GPIO0 - BAT_ADC (VCC / 3) */

/* LCD Pins (Waveshare ESP32-C6-LCD-1.9, ST7789V2) */
extern const GpioPin gpio_lcd_din;  /* GPIO4  - SPI MOSI */
extern const GpioPin gpio_lcd_clk;  /* GPIO5  - SPI SCLK */
extern const GpioPin gpio_lcd_dc;   /* GPIO6  - Data/Command */
extern const GpioPin gpio_lcd_cs;   /* GPIO7  - Chip Select */
extern const GpioPin gpio_lcd_rst;  /* GPIO14 - Reset */
extern const GpioPin gpio_lcd_bl;   /* GPIO15 - Backlight (PWM) */

/* SD Card Pins (Waveshare ESP32-C6-LCD-1.9, shared SPI2_HOST) */
extern const GpioPin gpio_sdcard_cs;   /* GPIO20 - SD Chip Select */
extern const GpioPin gpio_sdcard_miso; /* GPIO19 - SD MISO */

/* Touch Pins (Waveshare ESP32-C6-LCD-1.9, CST816S via I2C) */
extern const GpioPin gpio_touch_scl;   /* GPIO8  - I2C SCL */
extern const GpioPin gpio_touch_sda;   /* GPIO18 - I2C SDA */
extern const GpioPin gpio_touch_rst;   /* GPIO21 - Reset */
extern const GpioPin gpio_touch_int;   /* GPIO22 - Interrupt (active low) */

/* Flipper-style external header pins.
 * Current mapping is dedicated to the external CC1101 bring-up and overlaps with
 * some earlier Waveshare board assumptions for touch/SD.
 */
extern const GpioPin gpio_ext_pc0;
extern const GpioPin gpio_ext_pc1;
extern const GpioPin gpio_ext_pc3;
extern const GpioPin gpio_ext_pb2;
extern const GpioPin gpio_ext_pb3; /* CC1101 SCK  - GPIO2 */
extern const GpioPin gpio_ext_pa4; /* CC1101 CSN  - GPIO1 */
extern const GpioPin gpio_ext_pa6; /* CC1101 MISO - GPIO17 */
extern const GpioPin gpio_ext_pa7; /* CC1101 MOSI - GPIO3 */
extern const GpioPin gpio_cc1101_g0; /* CC1101 GDO0 - GPIO23 */
extern const GpioPin gpio_ibutton;
extern const GpioPin gpio_speaker;

#ifdef __cplusplus
}
#endif
