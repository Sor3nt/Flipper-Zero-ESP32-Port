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

#include <string.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/semphr.h>

static const char* TAG = "FuriHalDisplay";

/* How long a redraw is willing to wait for the shared SPI bus. If another
 * driver keeps it longer than this (a slow SD read, NFC acquisition) the frame
 * is dropped and retried on the next call: better a skipped frame than a GUI
 * that stops responding to buttons entirely. */
#define DISPLAY_BUS_LOCK_TIMEOUT_MS 30

/* Display dimensions from board config */
#define LCD_H_RES BOARD_LCD_H_RES
#define LCD_V_RES BOARD_LCD_V_RES

/* Flipper framebuffer dimensions */
#define FB_WIDTH  128
#define FB_HEIGHT 64

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
 * Reduces DMA buffer from full-frame (~110KB) to a small stripe. Larger stripes
 * mean fewer CASET/RASET/RAMWR round trips per frame; 32 lines keeps the buffer
 * at ~20KB of internal DMA RAM while cutting the transaction count from 20 to 5
 * for the Flipper UI area. */
#define STRIPE_HEIGHT 32

static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;
static bool panel_is_asleep = false; /* guards the sleep/wake pair */
static uint16_t* rgb565_buf = NULL; // STRIPE_HEIGHT lines only
static SemaphoreHandle_t lcd_flush_done = NULL;
static uint8_t x_scale_lut[SCALED_WIDTH];
static uint8_t y_scale_lut[SCALED_HEIGHT];

/* Frame de-duplication: the GUI re-commits the same 1KB mono framebuffer for
 * events that did not change a single pixel (status bar timers, view port
 * bookkeeping, repeated redraw requests). Pushing 110KB over SPI for those is
 * pure waste, so the previous buffer is kept and identical frames are dropped. */
static uint8_t fb_shadow[FB_WIDTH * FB_HEIGHT / 8];
static bool fb_shadow_valid = false;

/* Margin strips only depend on the background colour (and the fixed geometry),
 * not on the frame contents. Repainting them every frame used to cost ~40 tiny
 * SPI transactions per frame; now it happens once per colour change. */
static uint16_t margin_painted_color;
static bool margin_painted = false;

/* Lightweight frame timing, logged once per second, to keep the commit path
 * measurable on real hardware without a scope. */
static uint32_t dbg_frame_count;
static uint32_t dbg_frame_us_sum;
static uint32_t dbg_frame_us_max;
static uint32_t dbg_logged_frames;

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

bool furi_hal_display_draw_rgb565(
    uint16_t x, uint16_t y, uint16_t width, uint16_t height, const uint16_t* pixels) {
    if(!panel_handle || !io_handle || !pixels || !width || !height ||
       (uint32_t)x + width > LCD_H_RES || (uint32_t)y + height > LCD_V_RES) return false;
    if(!furi_hal_spi_bus_lock_timeout(DISPLAY_BUS_LOCK_TIMEOUT_MS)) return false;
    esp_err_t result = esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + width, y + height, pixels);
    /* Keep the bus lock until the DMA queue is empty; see furi_hal_display_sync(). */
    bool drained = furi_hal_display_sync();
    furi_hal_spi_bus_unlock();

    /* This raw panel write overwrote pixels the GUI commit path caches
     * assumptions about: the margin strips (painted once, only repainted on a
     * colour change) and the de-dup shadow of the last mono frame. Invalidate
     * both so the next furi_hal_display_commit() repaints the margins and does
     * not skip the frame as "unchanged". Without this, leaving a direct-draw
     * full-screen app (Sub-GHz spectrum, streaming) left its pixels in the
     * margins and the UI looked inset/smaller. */
    margin_painted = false;
    fb_shadow_valid = false;

    return result == ESP_OK && drained;
}

static void furi_hal_display_prepare_flush(void) {
    if(!lcd_flush_done) return;

    xSemaphoreTake(lcd_flush_done, 0);
}

static void furi_hal_display_wait_flush(void) {
    if(!lcd_flush_done) return;

    if(xSemaphoreTake(lcd_flush_done, pdMS_TO_TICKS(250)) != pdTRUE) {
        ESP_LOGW(TAG, "Timed out waiting for LCD flush");
    }
}

bool furi_hal_display_sync(void) {
    if(!io_handle) return false;

    /* ST7789 NOP: a parameter-only command with no payload. esp_lcd's SPI panel
     * IO drains every in-flight colour transaction before issuing it, so when
     * this returns the panel's DMA queue is empty and the SPI command register
     * is idle again.
     *
     * The caller must already hold the shared SPI bus mutex (furi_hal_spi_bus_lock),
     * otherwise another device can slip in between the drain and its own
     * transaction. That is exactly the espressif/esp-idf#17860 window. */
    esp_err_t err = esp_lcd_panel_io_tx_param(io_handle, 0x00, NULL, 0);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "LCD drain (NOP) failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
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
        furi_hal_display_prepare_flush();
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_H_RES, y + 1, line);
        furi_hal_display_wait_flush();
    }
    furi_hal_spi_bus_unlock();
    free(line);
}


void furi_hal_display_init(void) {
    ESP_LOGI(TAG, "Initializing display for %s", BOARD_NAME);
    furi_hal_spi_bus_init();
    if(!lcd_flush_done) {
        lcd_flush_done = xSemaphoreCreateBinary();
        ESP_ERROR_CHECK(lcd_flush_done ? ESP_OK : ESP_ERR_NO_MEM);
    }

    /* Initialize SPI bus */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = gpio_lcd_din.pin,
        .miso_io_num = gpio_sdcard_miso.pin,
        .sclk_io_num = gpio_lcd_clk.pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        /* Must cover the largest single transfer the panel driver queues:
         * one stripe of SCALED_WIDTH pixels, or a full-width margin strip
         * painted through the same scratch buffer. */
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
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &io_handle));

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
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0x01 /* SWRESET */, NULL, 0));
    vTaskDelay(pdMS_TO_TICKS(150));

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

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
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0x3A /* COLMOD */, &colmod, 1));
    }
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0x13 /* NORON */, NULL, 0));

    /* Turn on display */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    fg_color = BOARD_LCD_FG_COLOR;
    bg_color = BOARD_LCD_BG_COLOR;
    margin_painted = false;
    fb_shadow_valid = false;

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

    ESP_LOGI(TAG, "Display initialized (%dx%d, scaled %dx%d, stripe=%d lines, buf=%d bytes)",
             FB_WIDTH, FB_HEIGHT, SCALED_WIDTH, SCALED_HEIGHT, STRIPE_HEIGHT, (int)stripe_bytes);
}

/* Send one solid-color rectangle. The global bus lock is taken only for the
 * actual SPI transaction and released while waiting for DMA, so other devices
 * (SD, NFC, CC1101) can make progress between flushes. Returns false if the bus
 * could not be taken in time (the caller aborts the frame rather than blocking
 * the UI forever). */
static bool display_paint_rect_unlocked(
    size_t x, size_t y, size_t w, size_t h, uint16_t color) {
    if(w == 0 || h == 0 || w > LCD_H_RES) return true;

    for(size_t y_off = 0; y_off < h; y_off += STRIPE_HEIGHT) {
        size_t chunk_h = h - y_off;
        if(chunk_h > STRIPE_HEIGHT) chunk_h = STRIPE_HEIGHT;

        const size_t px_count = w * chunk_h;
        for(size_t i = 0; i < px_count; i++) rgb565_buf[i] = color;

        if(!furi_hal_spi_bus_lock_timeout(DISPLAY_BUS_LOCK_TIMEOUT_MS)) return false;
        furi_hal_display_prepare_flush();
        esp_lcd_panel_draw_bitmap(
            panel_handle, x, y + y_off, x + w, y + y_off + chunk_h, rgb565_buf);
        /* Drain the panel's DMA queue while the shared-bus mutex is still held.
         * esp_lcd releases the SPI bus as soon as it has queued the transfer,
         * so unlocking earlier would let CC1101/nRF24/NFC start a command on a
         * busy peripheral (espressif/esp-idf#17860). */
        furi_hal_display_wait_flush();
        furi_hal_display_sync();
        furi_hal_spi_bus_unlock();
    }

    return true;
}

void furi_hal_display_commit(const uint8_t* data, uint32_t size) {
    UNUSED(size);
    if(!panel_handle || !rgb565_buf || !data) return;

    /*
     * Stripe-based rendering: render STRIPE_HEIGHT lines into the small DMA
     * buffer, send via SPI, then render the next stripe.
     * Saves ~110KB of internal DMA-capable SRAM.
     *
     * u8g2 tile buffer layout:
     * 8 pages of 128 bytes each (128 columns × 8 pages = 1024 bytes)
     * Byte at [page * 128 + x]: bit n = pixel at (x, page*8 + n)
     * Bit 0 = top pixel in tile, bit 7 = bottom pixel in tile
     *
     * The global SPI bus mutex is taken per stripe, not for the whole frame: a
     * stripe is rendered into the scratch buffer first, then pushed with the
     * mutex held until the panel's DMA queue drains. Holding it across the
     * whole frame starved the SD card, NFC and sub-GHz drivers for hundreds of
     * milliseconds and made the UI look frozen. The ESP-IDF SPI master only
     * serialises transactions between devices *after* a transfer is queued; it
     * does not keep the bus busy while the LCD DMA is in flight, so the Furi
     * mutex must cover the drain as well (espressif/esp-idf#17860).
     */
    const int64_t commit_start_us = esp_timer_get_time();

    /* Repaint the margin strips only when the background colour actually
     * changed. They are static otherwise: the scaled UI never touches them. */
    if(!margin_painted || margin_painted_color != fg_color) {
        bool painted = true;
        if(MARGIN_Y > 0) {
            painted = display_paint_rect_unlocked(0, 0, LCD_H_RES, MARGIN_Y, fg_color);
            painted = painted && display_paint_rect_unlocked(
                0, MARGIN_Y + SCALED_HEIGHT,
                LCD_H_RES, LCD_V_RES - MARGIN_Y - SCALED_HEIGHT, fg_color);
        }
        if(painted && MARGIN_X > 0) {
            painted = display_paint_rect_unlocked(0, MARGIN_Y, MARGIN_X, SCALED_HEIGHT, fg_color);
            painted = painted && display_paint_rect_unlocked(
                MARGIN_X + SCALED_WIDTH, MARGIN_Y,
                LCD_H_RES - MARGIN_X - SCALED_WIDTH, SCALED_HEIGHT, fg_color);
        }
        if(!painted) {
            /* Bus busy: retry the margins on a later frame. */
            return;
        }
        margin_painted_color = fg_color;
        margin_painted = true;
    }

    /* Nothing changed since the last commit: the panel already shows this
     * exact frame, so skip the whole SPI transfer. */
    if(fb_shadow_valid && memcmp(fb_shadow, data, sizeof(fb_shadow)) == 0) {
        return;
    }

    bool frame_sent = true;
    for(size_t stripe_y = 0; stripe_y < SCALED_HEIGHT; stripe_y += STRIPE_HEIGHT) {
        size_t stripe_h = STRIPE_HEIGHT;
        if(stripe_y + stripe_h > SCALED_HEIGHT) {
            stripe_h = SCALED_HEIGHT - stripe_y;
        }

        /* Render stripe into buffer */
        const uint16_t fg = fg_color;
        const uint16_t bg = bg_color;
        for(size_t row = 0; row < stripe_h; row++) {
            const size_t sy = stripe_y + row;
            const uint8_t mono_y = y_scale_lut[sy];
            const size_t page = mono_y >> 3;
            const uint8_t bit_mask = 1U << (mono_y & 0x07);
            const uint8_t* src = &data[page * FB_WIDTH];
            uint16_t* dst = &rgb565_buf[row * SCALED_WIDTH];

            for(size_t sx = 0; sx < SCALED_WIDTH; sx++) {
                dst[sx] = (src[x_scale_lut[sx]] & bit_mask) ? bg : fg;
            }
        }

        /* DMA send this stripe: lock only for the register/transfer setup,
         * release while waiting for the DMA flush so the bus stays usable. */
        if(!furi_hal_spi_bus_lock_timeout(DISPLAY_BUS_LOCK_TIMEOUT_MS)) {
            /* Some other SPI user is hogging the bus for way too long. Drop
             * the rest of this frame instead of freezing the GUI thread. */
            ESP_LOGW(
                TAG,
                "commit: SPI bus busy >%dms, frame dropped",
                DISPLAY_BUS_LOCK_TIMEOUT_MS);
            frame_sent = false;
            break;
        }
        furi_hal_display_prepare_flush();
        esp_lcd_panel_draw_bitmap(
            panel_handle,
            MARGIN_X, MARGIN_Y + stripe_y,
            MARGIN_X + SCALED_WIDTH, MARGIN_Y + stripe_y + stripe_h,
            rgb565_buf);
        /* The bus mutex is held until the panel's DMA queue is empty; see
         * display_paint_rect_unlocked() for why (IDF #17860). */
        furi_hal_display_wait_flush();
        furi_hal_display_sync();
        furi_hal_spi_bus_unlock();
    }

    if(frame_sent) {
        /* Only remember the frame once every stripe actually reached the
         * panel, otherwise a dropped frame would be mistaken for "already
         * displayed" and never redrawn. */
        memcpy(fb_shadow, data, sizeof(fb_shadow));
        fb_shadow_valid = true;
    } else {
        fb_shadow_valid = false;
    }

    const uint32_t frame_us = (uint32_t)(esp_timer_get_time() - commit_start_us);
    dbg_frame_count++;
    dbg_frame_us_sum += frame_us;
    if(frame_us > dbg_frame_us_max) dbg_frame_us_max = frame_us;

    /* Once per second (roughly: every 60 commits) report the average and worst
     * commit time, so a slow frame is visible in the console log. */
    if(dbg_frame_count - dbg_logged_frames >= 60) {
        ESP_LOGI(
            TAG,
            "commit: avg=%lums max=%lums over %lu frames",
            (unsigned long)(dbg_frame_us_sum / dbg_frame_count / 1000U),
            (unsigned long)(dbg_frame_us_max / 1000U),
            (unsigned long)dbg_frame_count);
        dbg_logged_frames = dbg_frame_count;
    }
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
    if(!panel_handle || panel_is_asleep) return;
    furi_hal_spi_bus_lock();
    /* DISPOFF blanks output; SLPIN also stops the oscillator/DC-DC. GRAM kept. */
    esp_lcd_panel_disp_on_off(panel_handle, false);
    esp_lcd_panel_disp_sleep(panel_handle, true);
    furi_hal_spi_bus_unlock();
    panel_is_asleep = true;
    furi_hal_power_publish_idle_state(true);
}

void furi_hal_display_wakeup(void) {
    if(!panel_handle || !panel_is_asleep) return;
    furi_hal_spi_bus_lock();
    /* SLPOUT (driver waits ~100 ms to settle) then DISPON. */
    esp_lcd_panel_disp_sleep(panel_handle, false);
    esp_lcd_panel_disp_on_off(panel_handle, true);
    furi_hal_spi_bus_unlock();
    panel_is_asleep = false;
    furi_hal_power_publish_idle_state(false);
}

bool furi_hal_display_is_asleep(void) {
    return panel_is_asleep;
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
    if(color != fg_color) margin_painted = false;
    fg_color = color;
}

uint16_t furi_hal_display_get_fg_color(void) {
    return fg_color;
}

void furi_hal_display_set_bg_color(uint16_t color) {
    bg_color = color;
}

uint16_t furi_hal_display_get_bg_color(void) {
    return bg_color;
}
