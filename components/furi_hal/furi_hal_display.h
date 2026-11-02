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

/** Put the display panel into sleep mode (ST7789 SLPIN + display off).
 *
 * Intended for the power-off / deep-sleep path: the panel keeps drawing
 * ~10-15 mA while idle otherwise. Safe to call without holding the SPI bus
 * lock — it grabs the lock internally.
 */
void furi_hal_display_sleep(void);

/** Wake the display panel from sleep (ST7789 display on).
 *
 * Reverses furi_hal_display_sleep(). The frame buffer is retained in panel
 * RAM across sleep, so the last frame reappears with no re-init. Safe to call
 * when already awake (no-op) and without holding the SPI bus lock.
 */
void furi_hal_display_wakeup(void);

/** Called (from the waking thread) after the panel left sleep. The GUI uses it
 * to request a redraw: frames are not sent while the panel sleeps, so the first
 * frame after waking must be produced again. The callback must not block. */
typedef void (*FuriHalDisplayWakeCallback)(void* context);
void furi_hal_display_set_wake_callback(FuriHalDisplayWakeCallback callback, void* context);

/** True if the panel is currently in display-off/sleep (i.e. the screen has
 * timed out). Used to gate idle light sleep on "screen is off". */
bool furi_hal_display_is_asleep(void);
/** Atomically read sleep state and the transition counter used to detect brief wake cycles. */
bool furi_hal_display_get_sleep_state(uint32_t* transition_count);

/** Set the UI foreground color (the tint that fills the "ink" of every
 * monochrome u8g2 frame on this color port). Stored as RGB565, byte-swapped
 * for the ST7789 SPI byte order.
 *
 * Takes effect on the next frame commit (no full redraw needed).
 *
 * @param      color  RGB565 (byte-swapped) color value
 */
void furi_hal_display_set_fg_color(uint16_t color);

/** Get the current UI foreground color (RGB565 byte-swapped). Useful for
 * spectrum/animation drivers that need to derive the next frame's color.
 */
uint16_t furi_hal_display_get_fg_color(void);

/** Set/get the UI background color — fills the "set" mono pixels (the drawn
 * UI elements). Default is BOARD_LCD_BG_COLOR (typically black). Pairs with
 * fg_color to give the user full control of the bichromatic UI palette. */
void furi_hal_display_set_bg_color(uint16_t color);
uint16_t furi_hal_display_get_bg_color(void);

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

/** Draw an RGB565 (byte-swapped) rectangle straight to the panel and wait for
 * the DMA transfer to finish, so `data` may be reused as soon as this returns.
 *
 * For full-screen takeover apps holding gui_direct_draw_acquire(). Takes the
 * shared SPI bus lock internally. `data` must be in DMA-capable memory.
 * x1/y1 are exclusive, as in esp_lcd_panel_draw_bitmap().
 */
void furi_hal_display_blit(int x0, int y0, int x1, int y1, const void* data);

/** Like furi_hal_display_blit(), but `data` may be ANY memory (PSRAM included):
 * the pixels are copied in stripes through the HAL's own DMA buffer, which is
 * allocated once at boot. A takeover app therefore needs no internal DMA RAM of
 * its own, which is the allocation that fails when internal RAM is exhausted.
 * Blocks until the whole rectangle is on the panel. Returns false if the display
 * is not initialised or the rectangle is wider than the stripe buffer.
 */
bool furi_hal_display_blit_buffered(int x0, int y0, int x1, int y1, const uint16_t* data);

/** Forget what the HAL believes is on the panel, so the next commit repaints
 * the whole screen (margins included) instead of being skipped by the
 * frame-diff check. Call after drawing to the panel directly.
 */
void furi_hal_display_invalidate(void);

#ifdef __cplusplus
}
#endif
