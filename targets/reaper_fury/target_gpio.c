/**
 * @file target_gpio.c
 * GPIO pin definitions for reaper_fury (ESP32-S3 N16R8)
 * External GPIO pins exposed for expansion modules
 *
 * This file defines all available GPIO pins that can be controlled
 * through the GPIO application for testing and external module interfacing.
 */

#include <furi_hal_resources.h>

/**
 * GPIO Pins available for manual control and expansion module interfaces
 *
 * Structure:
 * - pin: Pointer to GpioPin constant (defined in furi_hal_resources.c)
 * - name: User-friendly pin name shown in GPIO app
 * - debug: Set to true to hide from GPIO app UI
 * - channel: ADC channel (0xFF = not ADC capable)
 */
const GpioPinRecord gpio_pins[] = {
    /* ---- External GPIO Pins (Available for expansion modules) ---- */
    {.pin = &gpio_ext_gpio_1, .name = "EXT_GPIO_1 (IO4)", .debug = false, .channel = 0xFF},
    {.pin = &gpio_ext_gpio_2, .name = "EXT_GPIO_2 (IO5)", .debug = false, .channel = 0xFF},
    {.pin = &gpio_ext_gpio_3, .name = "EXT_GPIO_3 (IO11)", .debug = false, .channel = 0xFF},
    {.pin = &gpio_ext_gpio_4, .name = "EXT_GPIO_4 (IO12)", .debug = false, .channel = 0xFF},
    {.pin = &gpio_ext_gpio_5, .name = "EXT_GPIO_5 (IO42)", .debug = false, .channel = 0xFF},

    /* ---- Built-in GPIO Pins (for reference/testing) ---- */
    {.pin = &gpio_ext_pc0, .name = "EXT_PC0", .debug = true, .channel = 0xFF},
    {.pin = &gpio_ext_pc1, .name = "EXT_PC1", .debug = true, .channel = 0xFF},
    {.pin = &gpio_ext_pb2, .name = "EXT_PB2", .debug = true, .channel = 0xFF},
    /* SPI pins are not exposed (CC1101 + NRF24 share SPI2 with LCD) */
    /* I2C pins are not exposed (NFC + Power ICs share I2C0) */
};

const size_t gpio_pins_count = sizeof(gpio_pins) / sizeof(gpio_pins[0]);
