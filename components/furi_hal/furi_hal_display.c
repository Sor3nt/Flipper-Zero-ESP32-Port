/**
 * @file furi_hal_display.c
 * Display HAL — board-driven configuration via boards/board.h
 *
 * Flipper GUI: 128x64 mono → aspect-fit scaled and centered on display
 */

#include "furi_hal_display.h"
#include "furi_hal_light.h"
#include "furi_hal_resources.h"
#include "furi_hal_spi_bus.h"
#include "boards/board.h"

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

static const char* TAG = "FuriHalDisplay";

/* Display dimensions from board config */
#define LCD_H_RES BOARD_LCD_H_RES
#define LCD_V_RES BOARD_LCD_V_RES

/* Flipper framebuffer dimensions */
#define FB_WIDTH  128
#define FB_HEIGHT 64

/* Scale the 128x64 framebuffer to the largest centered size that keeps aspect ratio. */
#if (LCD_H_RES * FB_HEIGHT) <= (LCD_V_RES * FB_WIDTH)
#define SCALED_WIDTH  LCD_H_RES
#define SCALED_HEIGHT ((LCD_H_RES * FB_HEIGHT) / FB_WIDTH)
#else
#define SCALED_HEIGHT LCD_V_RES
#define SCALED_WIDTH  ((LCD_V_RES * FB_WIDTH) / FB_HEIGHT)
#endif

/* Centering margins */
#define MARGIN_X ((LCD_H_RES - SCALED_WIDTH) / 2)
#define MARGIN_Y ((LCD_V_RES - SCALED_HEIGHT) / 2)

/* Colors from board config — fg_color is set at runtime based on reset reason */
#define BG_COLOR BOARD_LCD_BG_COLOR
static uint16_t fg_color;

/* SPI configuration from board config */
#define LCD_SPI_HOST   BOARD_LCD_SPI_HOST
#define LCD_SPI_FREQ   BOARD_LCD_SPI_FREQ_HZ
#define LCD_CMD_BITS   BOARD_LCD_CMD_BITS
#define LCD_PARAM_BITS BOARD_LCD_PARAM_BITS

/* Stripe-based rendering: render & DMA-send N lines at a time.
 * Reduces DMA buffer from full-frame (~100KB) to a small stripe (~5KB). */
#define STRIPE_HEIGHT 8

static esp_lcd_panel_handle_t panel_handle = NULL;
static uint16_t* rgb565_buf = NULL; // STRIPE_HEIGHT lines only
static SemaphoreHandle_t lcd_flush_done = NULL;
static uint8_t x_scale_lut[SCALED_WIDTH];
static uint8_t y_scale_lut[SCALED_HEIGHT];

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

static void furi_hal_display_wait_flush(void) {
    if(!lcd_flush_done) return;

    if(xSemaphoreTake(lcd_flush_done, pdMS_TO_TICKS(250)) != pdTRUE) {
        ESP_LOGW(TAG, "Timed out waiting for LCD flush");
    }
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
        .max_transfer_sz = SCALED_WIDTH * STRIPE_HEIGHT * sizeof(uint16_t),
    };
    esp_err_t spi_err = spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    /* ESP_ERR_INVALID_STATE is OK — bus may already be initialized by SubGHz on shared-bus boards */
    if(spi_err != ESP_OK && spi_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(spi_err);
    }

    /* Create panel IO (SPI) */
    esp_lcd_panel_io_handle_t io_handle = NULL;
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
    /* Hard-reset the ST7789 *before* creating the panel driver.
     * After a flash-reset the ESP32 GPIOs float during early boot, so the
     * display controller may never have seen a clean reset edge.  Toggling
     * the pin here (with generous timing) guarantees that the ST7789
     * registers — including MADCTL/color-order — are in their power-on
     * default state, regardless of how the ESP32 was reset. */
    gpio_config_t rst_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << gpio_lcd_rst.pin,
    };
    gpio_config(&rst_cfg);
    gpio_set_level((gpio_num_t)gpio_lcd_rst.pin, 0);   /* assert reset (active low) */
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)gpio_lcd_rst.pin, 1);   /* release reset */
    vTaskDelay(pdMS_TO_TICKS(120));                     /* ST7789 needs ≥5ms, use 120ms to be safe */

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    /* Reset and initialize (the esp_lcd driver reset is redundant now but harmless) */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    /* Display orientation and corrections from board config */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, BOARD_LCD_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, BOARD_LCD_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, BOARD_LCD_GAP_X, BOARD_LCD_GAP_Y));

    /* Turn on display */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    fg_color = BOARD_LCD_FG_COLOR;

    furi_hal_display_init_scale_lut();

    /* Allocate DMA-capable RGB565 stripe buffer (STRIPE_HEIGHT lines only) */
    size_t stripe_bytes = SCALED_WIDTH * STRIPE_HEIGHT * sizeof(uint16_t);
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

void furi_hal_display_commit(const uint8_t* data, uint32_t size) {
    UNUSED(size);
    if(!panel_handle || !rgb565_buf || !data) return;

    /*
     * Stripe-based rendering: render STRIPE_HEIGHT lines into the small DMA
     * buffer, send via SPI, then render the next stripe.
     * Saves ~95KB of internal DMA-capable SRAM.
     *
     * u8g2 tile buffer layout:
     * 8 pages of 128 bytes each (128 columns × 8 pages = 1024 bytes)
     * Byte at [page * 128 + x]: bit n = pixel at (x, page*8 + n)
     * Bit 0 = top pixel in tile, bit 7 = bottom pixel in tile
     */
    furi_hal_spi_bus_lock();

    for(size_t stripe_y = 0; stripe_y < SCALED_HEIGHT; stripe_y += STRIPE_HEIGHT) {
        size_t stripe_h = STRIPE_HEIGHT;
        if(stripe_y + stripe_h > SCALED_HEIGHT) {
            stripe_h = SCALED_HEIGHT - stripe_y;
        }

        /* Render stripe into buffer */
        for(size_t row = 0; row < stripe_h; row++) {
            const size_t sy = stripe_y + row;
            const uint8_t mono_y = y_scale_lut[sy];
            const size_t page = mono_y >> 3;
            const uint8_t bit_mask = 1U << (mono_y & 0x07);
            uint16_t* dst = &rgb565_buf[row * SCALED_WIDTH];

            for(size_t sx = 0; sx < SCALED_WIDTH; sx++) {
                const uint8_t mono_x = x_scale_lut[sx];
                const bool pixel_set = (data[page * FB_WIDTH + mono_x] & bit_mask) != 0;
                dst[sx] = pixel_set ? BG_COLOR : fg_color;
            }
        }

        /* DMA send this stripe */
        furi_hal_display_prepare_flush();
        esp_lcd_panel_draw_bitmap(
            panel_handle,
            MARGIN_X, MARGIN_Y + stripe_y,
            MARGIN_X + SCALED_WIDTH, MARGIN_Y + stripe_y + stripe_h,
            rgb565_buf);
        furi_hal_display_wait_flush();
    }

    furi_hal_spi_bus_unlock();
}

void furi_hal_display_set_backlight(uint8_t brightness) {
    furi_hal_light_set(LightBacklight, brightness);
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
