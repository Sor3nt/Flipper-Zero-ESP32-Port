/**
 * @file furi_hal_display.h
 * Display HAL API (ESP32-C6, ST7789V2 via esp_lcd)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize display hardware (ST7789V2 via SPI + esp_lcd)
 */
void furi_hal_display_init(void);

/** Commit display buffer to screen
 *
 * Converts u8g2 mono framebuffer (128x64, 1bpp tile format) to
 * RGB565 with aspect-fit scaling and sends it to ST7789V2 via DMA.
 *
 * @param      data  pointer to u8g2 framebuffer data
 * @param      size  size of framebuffer data in bytes
 */
void furi_hal_display_commit(const uint8_t* data, uint32_t size);

/** Set display backlight brightness
 *
 * @param      brightness  brightness level [0-255]
 */
void furi_hal_display_set_backlight(uint8_t brightness);

#ifdef __cplusplus
}
#endif
