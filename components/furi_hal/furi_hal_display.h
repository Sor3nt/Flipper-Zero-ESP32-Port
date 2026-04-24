/**
 * @file furi_hal_display.h
 * Display HAL API (ESP32-C6, ST7789V2 via esp_lcd)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <esp_lcd_types.h>

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

/** Get native panel dimensions (post swap_xy). Intended for full-screen
 * takeover apps (e.g. game emulators) that bypass the 128x64 framebuffer.
 */
uint16_t furi_hal_display_get_h_res(void);
uint16_t furi_hal_display_get_v_res(void);

/** Get the underlying esp_lcd panel handle for direct full-screen drawing.
 *
 * The returned handle must only be used while the caller holds the SPI bus
 * lock (furi_hal_spi_bus_lock) and has acquired fullscreen access via the
 * GUI service (gui_direct_draw_acquire). The Furi GUI HAL keeps rendering
 * its 128x64 framebuffer, so callers that do not pause the GUI will flicker.
 */
esp_lcd_panel_handle_t furi_hal_display_get_panel_handle(void);

#ifdef __cplusplus
}
#endif
