/**
 * @file furi_hal_display.c
 * Display HAL — board-driven configuration via boards/board.h
 *
 * Flipper GUI: 128x64 mono → aspect-fit scaled and centered on display
 */

#include "furi_hal_display.h"
#include "furi_hal_light.h"
#include "furi_hal_power.h"
#include "furi_hal_resources.h"
#include "furi_hal_spi_bus.h"
#include "boards/board.h"

#include <furi.h>
#include <string.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static const char* TAG = "FuriHalDisplay";

/* Display dimensions from board config */
#define LCD_H_RES BOARD_LCD_H_RES
#define LCD_V_RES BOARD_LCD_V_RES

/* Flipper framebuffer dimensions */
#define FB_WIDTH  128
#define FB_HEIGHT 64
#define FB_SIZE_BYTES (FB_WIDTH * FB_HEIGHT / 8)

/* Always-on left/right inset (in physical LCD pixels) kept as plain background
 * color so the UI never sits flush against the screen's left/right edges. A
 * board may override this in its board.h; otherwise this default applies
 * everywhere. Set to 0 to disable. */
#ifndef BOARD_LCD_SIDE_MARGIN
#define BOARD_LCD_SIDE_MARGIN 4
#endif
#define DISPLAY_SIDE_MARGIN BOARD_LCD_SIDE_MARGIN

/* Horizontal resolution actually available to the UI after reserving the
 * left/right margins. The aspect-fit below scales into this, then the result
 * is centered across the full LCD resolution — so the leftover on each side
 * is always >= the margin and is painted with the background color. */
#define USABLE_H_RES (LCD_H_RES - 2 * DISPLAY_SIDE_MARGIN)

/* Scale the 128x64 framebuffer to the largest centered size that keeps aspect ratio. */
#if (USABLE_H_RES * FB_HEIGHT) <= (LCD_V_RES * FB_WIDTH)
#define SCALED_WIDTH  USABLE_H_RES
#define SCALED_HEIGHT ((USABLE_H_RES * FB_HEIGHT) / FB_WIDTH)
#else
#define SCALED_HEIGHT LCD_V_RES
#define SCALED_WIDTH  ((LCD_V_RES * FB_WIDTH) / FB_HEIGHT)
#endif

/* Centering margins */
#define MARGIN_X ((LCD_H_RES - SCALED_WIDTH) / 2)
#define MARGIN_Y ((LCD_V_RES - SCALED_HEIGHT) / 2)

/* Colors from board config — both are set at runtime so the user can pick
 * UI Background (fg_color, the field that fills "unset" mono pixels) and
 * UI Foreground (bg_color, what fills "set" pixels = drawn UI elements). */
static uint16_t fg_color;
static uint16_t bg_color;

/* SPI configuration from board config */
#define LCD_SPI_HOST   BOARD_LCD_SPI_HOST
#define LCD_SPI_FREQ   BOARD_LCD_SPI_FREQ_HZ
#define LCD_CMD_BITS   BOARD_LCD_CMD_BITS
#define LCD_PARAM_BITS BOARD_LCD_PARAM_BITS

/* Stripe-based rendering: render & DMA-send N lines at a time.
 * Reduces DMA buffer from full-frame (~100KB) to a small stripe (~5KB). */
#define STRIPE_HEIGHT 8

static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_panel_io_handle_t panel_io_handle = NULL;
static bool panel_is_asleep = false; /* guards the sleep/wake pair */
static uint32_t panel_sleep_transition_count = 0;
static uint16_t* rgb565_buf = NULL; // STRIPE_HEIGHT lines only
static SemaphoreHandle_t lcd_flush_done = NULL;
/* Owns rgb565_buf: commit() renders into it and furi_hal_display_blit_buffered()
 * bounces caller pixels through it. The GUI can still be finishing a commit when
 * an app takes the display over (gui_direct_draw_acquire only sets a flag). */
static SemaphoreHandle_t display_buf_lock = NULL;
static uint8_t x_scale_lut[SCALED_WIDTH];
static uint8_t y_scale_lut[SCALED_HEIGHT];

/* Frame-diff + dirty-margin state, so commit() does minimal work (and holds
 * the shared SPI bus lock for minimal time) when nothing actually changed.
 * See furi_hal_display_commit() for why this matters on shared-bus boards. */
static uint8_t last_fb[FB_SIZE_BYTES];
static volatile bool last_fb_valid = false;
static volatile bool margins_dirty = true;

static bool furi_hal_display_flush_done_callback(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t* edata,
    void* user_ctx) {
    UNUSED(panel_io);
    UNUSED(edata);
    UNUSED(user_ctx);

    BaseType_t yield = pdFALSE;
    if(lcd_flush_done) {
        xSemaphoreGiveFromISR(lcd_flush_done, &yield);
    }

    return yield == pdTRUE;
}

static void furi_hal_display_prepare_flush(void) {
    if(!lcd_flush_done) return;

    xSemaphoreTake(lcd_flush_done, 0);
}

static bool furi_hal_display_wait_flush(void) {
    if(!lcd_flush_done) return false;
    if(xSemaphoreTake(lcd_flush_done, pdMS_TO_TICKS(250)) == pdTRUE) return true;

    ESP_LOGW(TAG, "Timed out waiting for LCD flush; draining queued color transfer");
    if(panel_io_handle) {
        /* esp_lcd_panel_io_tx_param waits for queued color DMA to finish. ST7789
         * NOP has no display side effect; its completion is the bus/buffer fence. */
        const esp_err_t drain_err =
            esp_lcd_panel_io_tx_param(panel_io_handle, 0x00 /* NOP */, NULL, 0);
        if(drain_err == ESP_OK) {
            (void)xSemaphoreTake(lcd_flush_done, 0);
            return true;
        }
        ESP_LOGE(
            TAG,
            "LCD DMA drain failed: %s; waiting for completion before releasing SPI",
            esp_err_to_name(drain_err));
    }

    /* Never release the shared SPI lock or recycle the DMA buffer while the
     * transfer completion is unknown. A failed callback must fail closed. */
    return xSemaphoreTake(lcd_flush_done, portMAX_DELAY) == pdTRUE;
}

/* The caller holds the shared SPI bus lock through draw submission and DMA
 * completion so radio/SD transfers cannot overlap the LCD transfer. */
static bool furi_hal_display_send_bitmap(
    int x0,
    int y0,
    int x1,
    int y1,
    const void* data) {
    furi_hal_display_prepare_flush();
    const esp_err_t draw_err = esp_lcd_panel_draw_bitmap(panel_handle, x0, y0, x1, y1, data);
    if(draw_err != ESP_OK) {
        ESP_LOGW(TAG, "LCD bitmap transfer was not queued: %s", esp_err_to_name(draw_err));
        return false;
    }
    return furi_hal_display_wait_flush();
}

static void furi_hal_display_init_scale_lut(void) {
    for(size_t x = 0; x < SCALED_WIDTH; x++) {
        x_scale_lut[x] = (x * FB_WIDTH) / SCALED_WIDTH;
    }

    for(size_t y = 0; y < SCALED_HEIGHT; y++) {
        y_scale_lut[y] = (y * FB_HEIGHT) / SCALED_HEIGHT;
    }
}

static void display_fill_color(uint16_t color) {
    uint16_t* line = heap_caps_malloc(LCD_H_RES * sizeof(uint16_t), MALLOC_CAP_DMA);
    if(!line) return;
    for(int i = 0; i < LCD_H_RES; i++) line[i] = color;
    furi_hal_spi_bus_lock();
    for(int y = 0; y < LCD_V_RES; y++) {
        if(!furi_hal_display_send_bitmap(0, y, LCD_H_RES, y + 1, line)) break;
    }
    furi_hal_spi_bus_unlock();
    free(line);
}

/* Send one already-rendered stripe to the panel. Takes the shared SPI bus
 * lock only for this one short DMA transfer (worst case: one stripe's worth
 * of pixels, a few KB at the panel's SPI clock -- on the order of a
 * millisecond), instead of the lock being held for the whole frame. This is
 * what lets other shared-bus users (CC1101/NRF24 register transactions, SD
 * card sector ops) get scheduled in between stripes rather than queuing
 * behind one multi-stripe hold. */
static bool display_flush_stripe(int x0, int y0, int x1, int y1) {
    furi_hal_spi_bus_lock();
    const bool complete = furi_hal_display_send_bitmap(x0, y0, x1, y1, rgb565_buf);
    furi_hal_spi_bus_unlock();
    return complete;
}

/* Paint a solid-color rectangle using rgb565_buf as scratch. Chunks vertically
 * at STRIPE_HEIGHT to stay within the buffer's capacity (STRIPE_HEIGHT rows ×
 * SCALED_WIDTH cols, reused here at full LCD width). Used by the commit path
 * to keep the LCD margins in sync with fg_color changes -- only called when
 * margins_dirty is set, not on every frame. */
static bool display_paint_rect(
    size_t x, size_t y, size_t w, size_t h, uint16_t color) {
    if(w == 0 || h == 0 || w > LCD_H_RES) return true;

    for(size_t y_off = 0; y_off < h; y_off += STRIPE_HEIGHT) {
        size_t chunk_h = h - y_off;
        if(chunk_h > STRIPE_HEIGHT) chunk_h = STRIPE_HEIGHT;

        const size_t px_count = w * chunk_h;
        for(size_t i = 0; i < px_count; i++) rgb565_buf[i] = color;

        if(!display_flush_stripe(
               (int)x, (int)(y + y_off), (int)(x + w), (int)(y + y_off + chunk_h))) {
            return false;
        }
    }
    return true;
}

void furi_hal_display_init(void) {
    ESP_LOGI(TAG, "Initializing display for %s", BOARD_NAME);
    furi_hal_spi_bus_init();
    if(!lcd_flush_done) {
        lcd_flush_done = xSemaphoreCreateBinary();
        ESP_ERROR_CHECK(lcd_flush_done ? ESP_OK : ESP_ERR_NO_MEM);
    }
    if(!display_buf_lock) {
        display_buf_lock = xSemaphoreCreateMutex();
        ESP_ERROR_CHECK(display_buf_lock ? ESP_OK : ESP_ERR_NO_MEM);
    }

    /* Initialize SPI bus */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = gpio_lcd_din.pin,
        .miso_io_num = gpio_sdcard_miso.pin,
        .sclk_io_num = gpio_lcd_clk.pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * STRIPE_HEIGHT * sizeof(uint16_t),
    };
    esp_err_t spi_err = spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    /* ESP_ERR_INVALID_STATE is OK — bus may already be initialized by SubGHz on shared-bus boards */
    if(spi_err != ESP_OK && spi_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(spi_err);
    }

    /* Create panel IO (SPI) */
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = gpio_lcd_dc.pin,
        .cs_gpio_num = gpio_lcd_cs.pin,
        .pclk_hz = LCD_SPI_FREQ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 1,
        .on_color_trans_done = furi_hal_display_flush_done_callback,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &panel_io_handle));

    /* Create ST7789 panel */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = gpio_lcd_rst.pin,
#if defined(BOARD_LCD_COLOR_ORDER_BGR) && BOARD_LCD_COLOR_ORDER_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
    };
    /* --- Bring the ST7789 to a known-clean state, every boot --------------
     * A software reset (esp_restart) does NOT power-cycle the display: the
     * ST7789 keeps every register — MADCTL/colour-order, COLMOD, inversion,
     * rotation. That happens after an `esptool` flash *and* after any other
     * firmware that configured the panel differently ran before us. If any of
     * that lingers the R/B channels end up swapped — Flipper orange shows up as
     * blue — until the user pulls the battery (a real cold boot). So we do what
     * a thorough driver (TFT_eSPI) does on every init: hardware-reset pulse →
     * SWRESET command → full panel init → re-assert COLMOD. After that the panel
     * is in *our* configuration regardless of what ran before. */

    /* 1) Hardware reset pulse on RESX (HIGH → LOW → HIGH, generous timing). */
    gpio_config_t rst_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << gpio_lcd_rst.pin,
    };
    gpio_config(&rst_cfg);
    gpio_set_level((gpio_num_t)gpio_lcd_rst.pin, 1);   /* ensure a clean falling edge */
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)gpio_lcd_rst.pin, 0);   /* assert reset (active low) */
    vTaskDelay(pdMS_TO_TICKS(20));                      /* RESX min low is 10us; be generous */
    gpio_set_level((gpio_num_t)gpio_lcd_rst.pin, 1);   /* release reset */
    vTaskDelay(pdMS_TO_TICKS(150));                     /* ST7789: wait ≥120ms after reset */

    /* 2) Software reset (0x01) — re-loads all registers to factory defaults
     *    even if the RESX pulse above didn't fully take (e.g. a glitch on the
     *    line right after the ESP32 digital reset). The display still draws
     *    pixels in that case, so this command reaches it just fine. */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_handle, 0x01 /* SWRESET */, NULL, 0));
    vTaskDelay(pdMS_TO_TICKS(150));

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_handle, &panel_config, &panel_handle));

    /* 3) esp_lcd does another RESX pulse, then SLPOUT + COLMOD + MADCTL. */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    /* Display orientation and corrections from board config (these (re)write
     * MADCTL with our colour order + mirror/swap bits, and INVON/INVOFF). */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, BOARD_LCD_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, BOARD_LCD_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, BOARD_LCD_GAP_X, BOARD_LCD_GAP_Y));

    /* 4) Belt-and-suspenders: pin down pixel format + normal display mode.
     *    COLMOD 0x55 = 16 bits/pixel (RGB565) — matches bits_per_pixel=16 above;
     *    NORON (0x13) = normal display mode (not partial/idle). Both are no-ops
     *    when the state is already right and cost nothing. */
    {
        const uint8_t colmod = 0x55;
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_handle, 0x3A /* COLMOD */, &colmod, 1));
    }
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_handle, 0x13 /* NORON */, NULL, 0));

    /* Turn on display */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    fg_color = BOARD_LCD_FG_COLOR;
    bg_color = BOARD_LCD_BG_COLOR;

    furi_hal_display_init_scale_lut();

    /* Allocate DMA-capable RGB565 stripe buffer (STRIPE_HEIGHT lines only).
     * Sized to the full LCD width so the same scratch buffer can paint the
     * full-width top/bottom margin strips as well as the scaled UI stripes. */
    size_t stripe_bytes = LCD_H_RES * STRIPE_HEIGHT * sizeof(uint16_t);
    rgb565_buf = heap_caps_malloc(stripe_bytes, MALLOC_CAP_DMA);
    if(!rgb565_buf) {
        ESP_LOGE(TAG, "Failed to allocate RGB565 stripe buffer (%d bytes)",
                 (int)stripe_bytes);
        return;
    }

    /* Clear entire screen to background (unset pixels = FG) */
    display_fill_color(fg_color);

    /* First real commit() must repaint the margins and cannot rely on the
     * (nonexistent) previous frame for diffing. */
    margins_dirty = true;
    last_fb_valid = false;

    ESP_LOGI(TAG, "Display initialized (%dx%d, scaled %dx%d, stripe=%d lines, buf=%d bytes)",
             FB_WIDTH, FB_HEIGHT, SCALED_WIDTH, SCALED_HEIGHT, STRIPE_HEIGHT, (int)stripe_bytes);
}

static void furi_hal_display_commit_locked(const uint8_t* data, uint32_t size);

void furi_hal_display_commit(const uint8_t* data, uint32_t size) {
    if(!display_buf_lock) return;
    xSemaphoreTake(display_buf_lock, portMAX_DELAY);
    furi_hal_display_commit_locked(data, size);
    xSemaphoreGive(display_buf_lock);
}

static void furi_hal_display_commit_locked(const uint8_t* data, uint32_t size) {
    /* The renderer and the frame diff below read a whole 128x64 mono frame. */
    if(!panel_handle || !rgb565_buf || !data || size < (uint32_t)FB_SIZE_BYTES) return;

    /* Panel in SLPIN with the backlight off: nothing is visible, so do not
     * render and DMA ~100 KB over the shared SPI bus (that also takes insomnia
     * and blocks light sleep). furi_hal_display_wakeup() invalidates the
     * frame cache, so the first frame after waking is always sent in full. */
    if(furi_hal_display_is_asleep()) return;

    /* Nothing changed since last frame and the margins are already correct:
     * skip the whole commit. This is what keeps an idle/static screen from
     * re-sending ~97KB of pixels (and re-taking the shared SPI lock dozens of
     * times) every single GUI redraw tick while nothing on screen moved. */
    if(last_fb_valid && !margins_dirty && memcmp(last_fb, data, FB_SIZE_BYTES) == 0) {
        return;
    }

    /*
     * Stripe-based rendering: render STRIPE_HEIGHT lines into the small DMA
     * buffer, send via SPI, then render the next stripe.
     * Saves ~95KB of internal DMA-capable SRAM.
     *
     * u8g2 tile buffer layout:
     * 8 pages of 128 bytes each (128 columns × 8 pages = 1024 bytes)
     * Byte at [page * 128 + x]: bit n = pixel at (x, page*8 + n)
     * Bit 0 = top pixel in tile, bit 7 = bottom pixel in tile
     *
     * IMPORTANT (shared-bus boards): each stripe is rendered into rgb565_buf
     * WITHOUT holding the SPI bus lock (render is pure CPU, no SPI traffic),
     * then display_flush_stripe() takes the lock only for that one stripe's
     * DMA transfer. This bounds any single lock hold to roughly one stripe's
     * transfer time (a dozen stripes per frame, ~1ms each at 40MHz) instead
     * of the whole ~20-35ms frame, so CC1101/NRF24/SD can interleave their
     * own short transactions between stripes rather than queuing behind one
     * long hold. Do not move SPI bus locking out to cover the whole function
     * again — that reintroduces the delay this structure fixes. */

    /* Repaint the margin strips with the current fg_color only when something
     * actually requires it (first commit, or a UI/background color change) —
     * not on every single frame. Without the margins_dirty guard this would
     * run ~42 small draw_bitmap calls every frame for no visual benefit, each
     * one taking the shared SPI lock. */
    if(margins_dirty) {
        if(MARGIN_Y > 0) {
            if(!display_paint_rect(0, 0, LCD_H_RES, MARGIN_Y, fg_color)) return;
            if(!display_paint_rect(
                0, MARGIN_Y + SCALED_HEIGHT,
                LCD_H_RES, LCD_V_RES - MARGIN_Y - SCALED_HEIGHT, fg_color)) {
                return;
            }
        }
        if(MARGIN_X > 0) {
            if(!display_paint_rect(0, MARGIN_Y, MARGIN_X, SCALED_HEIGHT, fg_color)) return;
            if(!display_paint_rect(
                MARGIN_X + SCALED_WIDTH, MARGIN_Y,
                LCD_H_RES - MARGIN_X - SCALED_WIDTH, SCALED_HEIGHT, fg_color)) {
                return;
            }
        }
        margins_dirty = false;
    }

    for(size_t stripe_y = 0; stripe_y < SCALED_HEIGHT; stripe_y += STRIPE_HEIGHT) {
        size_t stripe_h = STRIPE_HEIGHT;
        if(stripe_y + stripe_h > SCALED_HEIGHT) {
            stripe_h = SCALED_HEIGHT - stripe_y;
        }

        /* Render stripe into buffer (no SPI lock held here). Consecutive
         * destination rows that sample the same mono source row (always true
         * when SCALED_HEIGHT > FB_HEIGHT, i.e. upscaling) are memcpy'd from the
         * previous row instead of re-walking every column, since the result
         * is identical — this roughly halves render CPU time at this board's
         * ~2.4x vertical scale factor. */
        uint8_t prev_mono_y = 0xFF;
        uint16_t* prev_row = NULL;
        for(size_t row = 0; row < stripe_h; row++) {
            const size_t sy = stripe_y + row;
            const uint8_t mono_y = y_scale_lut[sy];
            uint16_t* dst = &rgb565_buf[row * SCALED_WIDTH];

            if(prev_row && mono_y == prev_mono_y) {
                memcpy(dst, prev_row, SCALED_WIDTH * sizeof(uint16_t));
            } else {
                const size_t page = mono_y >> 3;
                const uint8_t bit_mask = 1U << (mono_y & 0x07);
                for(size_t sx = 0; sx < SCALED_WIDTH; sx++) {
                    const uint8_t mono_x = x_scale_lut[sx];
                    const bool pixel_set = (data[page * FB_WIDTH + mono_x] & bit_mask) != 0;
                    dst[sx] = pixel_set ? bg_color : fg_color;
                }
            }
            prev_mono_y = mono_y;
            prev_row = dst;
        }

        /* DMA send this stripe -- takes and releases the shared SPI lock
         * internally, just for this one transfer. */
        if(!display_flush_stripe(
               MARGIN_X, MARGIN_Y + stripe_y,
               MARGIN_X + SCALED_WIDTH, MARGIN_Y + stripe_y + stripe_h)) {
            last_fb_valid = false;
            return;
        }

        /* Give any same/higher-priority task (radio worker, SD I/O) a chance
         * to run between stripes even on a single-core-bound build; harmless
         * no-op if nothing else is runnable. */
        taskYIELD();
    }

    memcpy(last_fb, data, FB_SIZE_BYTES);
    last_fb_valid = true;
}

void furi_hal_display_blit(int x0, int y0, int x1, int y1, const void* data) {
    if(!panel_handle || !data) return;
    furi_hal_spi_bus_lock();
    (void)furi_hal_display_send_bitmap(x0, y0, x1, y1, data);
    furi_hal_spi_bus_unlock();
}

bool furi_hal_display_blit_buffered(int x0, int y0, int x1, int y1, const uint16_t* data) {
    const int w = x1 - x0;
    const int h = y1 - y0;
    if(!panel_handle || !rgb565_buf || !display_buf_lock || !data || w <= 0 || h <= 0) {
        return false;
    }
    const int rows_per_chunk = (int)((LCD_H_RES * STRIPE_HEIGHT) / (unsigned)w);
    if(rows_per_chunk < 1) return false;

    xSemaphoreTake(display_buf_lock, portMAX_DELAY);
    bool success = true;
    for(int row = 0; row < h; row += rows_per_chunk) {
        const int rows = (row + rows_per_chunk > h) ? (h - row) : rows_per_chunk;
        memcpy(rgb565_buf, data + (size_t)row * w, (size_t)rows * w * sizeof(uint16_t));
        if(!display_flush_stripe(x0, y0 + row, x1, y0 + row + rows)) {
            success = false;
            break;
        }
    }
    xSemaphoreGive(display_buf_lock);
    return success;
}

void furi_hal_display_invalidate(void) {
    last_fb_valid = false;
    margins_dirty = true;
}

void furi_hal_display_set_backlight(uint8_t brightness) {
    /* brightness 0 means "screen off" here, so pair it with panel sleep; GRAM
     * is retained, so waking shows the last frame with no re-init. */
    if(brightness == 0) {
        furi_hal_light_set(LightBacklight, 0);
        furi_hal_display_sleep();
    } else {
        furi_hal_display_wakeup();
        furi_hal_light_set(LightBacklight, brightness);
    }
}

void furi_hal_display_sleep(void) {
    if(!panel_handle) return;
    /* A prior screen-off grant must not carry across a new dim transition. */
    furi_hal_power_allow_light_sleep(false);
    furi_hal_spi_bus_lock();
    if(!panel_is_asleep) {
        /* DISPOFF blanks output; SLPIN also stops the oscillator/DC-DC. GRAM kept. */
        esp_lcd_panel_disp_on_off(panel_handle, false);
        esp_lcd_panel_disp_sleep(panel_handle, true);
        FURI_CRITICAL_ENTER();
        panel_is_asleep = true;
        panel_sleep_transition_count++;
        FURI_CRITICAL_EXIT();
    }
    furi_hal_spi_bus_unlock();
}

static FuriHalDisplayWakeCallback display_wake_callback = NULL;
static void* display_wake_context = NULL;

void furi_hal_display_set_wake_callback(FuriHalDisplayWakeCallback callback, void* context) {
    FURI_CRITICAL_ENTER();
    display_wake_callback = callback;
    display_wake_context = context;
    FURI_CRITICAL_EXIT();
}

void furi_hal_display_wakeup(void) {
    if(!panel_handle) return;
    /* Close admission before waking the panel; this also resets the idle grace period. */
    furi_hal_power_allow_light_sleep(false);
    furi_hal_spi_bus_lock();
    if(panel_is_asleep) {
        /* SLPOUT (driver waits ~100 ms to settle) then DISPON. */
        esp_lcd_panel_disp_sleep(panel_handle, false);
        esp_lcd_panel_disp_on_off(panel_handle, true);
        FURI_CRITICAL_ENTER();
        panel_is_asleep = false;
        panel_sleep_transition_count++;
        FURI_CRITICAL_EXIT();
    }
    furi_hal_spi_bus_unlock();

    /* The panel was blanked/asleep; GRAM content may be stale relative to
     * whatever the GUI thinks is currently on screen. Force the next
     * commit() to redraw unconditionally instead of being skipped by the
     * frame-diff check above. */
    last_fb_valid = false;

    /* Frames are dropped while asleep (see commit()), so ask for a fresh one. */
    FURI_CRITICAL_ENTER();
    FuriHalDisplayWakeCallback callback = display_wake_callback;
    void* context = display_wake_context;
    FURI_CRITICAL_EXIT();
    if(callback) callback(context);
}

bool furi_hal_display_is_asleep(void) {
    return furi_hal_display_get_sleep_state(NULL);
}

bool furi_hal_display_get_sleep_state(uint32_t* transition_count) {
    FURI_CRITICAL_ENTER();
    const bool is_asleep = panel_is_asleep;
    if(transition_count) *transition_count = panel_sleep_transition_count;
    FURI_CRITICAL_EXIT();
    return is_asleep;
}

uint16_t furi_hal_display_get_h_res(void) {
    return LCD_H_RES;
}

uint16_t furi_hal_display_get_v_res(void) {
    return LCD_V_RES;
}

esp_lcd_panel_handle_t furi_hal_display_get_panel_handle(void) {
    return panel_handle;
}

void furi_hal_display_set_fg_color(uint16_t color) {
    fg_color = color;
    margins_dirty = true;
    last_fb_valid = false;
}

uint16_t furi_hal_display_get_fg_color(void) {
    return fg_color;
}

void furi_hal_display_set_bg_color(uint16_t color) {
    bg_color = color;
    last_fb_valid = false;
}

uint16_t furi_hal_display_get_bg_color(void) {
    return bg_color;
}
