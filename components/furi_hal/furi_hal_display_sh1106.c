/**
 * SH1106/SSD1306 128x64 I2C display HAL. The GUI framebuffer already uses
 * the controllers' native page/column 1-bpp layout, so commits need no conversion.
 */

#include "furi_hal_display.h"
#include "furi_hal_power.h"
#include "boards/board.h"

#include <furi.h>
#include <string.h>
#include <driver/gpio.h>
#include <driver/i2c.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define FB_WIDTH  128U
#define FB_HEIGHT 64U
#define FB_SIZE   (FB_WIDTH * FB_HEIGHT / 8U)
#define I2C_TIMEOUT_TICKS pdMS_TO_TICKS(100)

#if defined(BOARD_DISPLAY_SSD1306)
static const char* TAG = "FuriHalSSD1306";
#else
static const char* TAG = "FuriHalSH1106";
#endif
static uint16_t fg_color = BOARD_LCD_FG_COLOR;
static uint16_t bg_color = BOARD_LCD_BG_COLOR;
static bool display_ready = false;

static esp_err_t sh1106_write(const uint8_t* data, size_t size) {
    return i2c_master_write_to_device(
        BOARD_DISPLAY_I2C_PORT,
        BOARD_DISPLAY_I2C_ADDR,
        data,
        size,
        I2C_TIMEOUT_TICKS);
}

static esp_err_t sh1106_commands(const uint8_t* commands, size_t count) {
    uint8_t buffer[32];
    if(count + 1U > sizeof(buffer)) return ESP_ERR_INVALID_SIZE;
    buffer[0] = 0x00;
    memcpy(&buffer[1], commands, count);
    return sh1106_write(buffer, count + 1U);
}

void furi_hal_display_init(void) {
    ESP_LOGI(
        TAG,
        "Initializing %s at I2C address 0x%02X (SDA=%d SCL=%d)",
        BOARD_NAME,
        BOARD_DISPLAY_I2C_ADDR,
        BOARD_PIN_DISPLAY_SDA,
        BOARD_PIN_DISPLAY_SCL);

    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_PIN_DISPLAY_SDA,
        .scl_io_num = BOARD_PIN_DISPLAY_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BOARD_DISPLAY_I2C_FREQ_HZ,
        .clk_flags = 0,
    };
    ESP_ERROR_CHECK(i2c_param_config(BOARD_DISPLAY_I2C_PORT, &config));
    esp_err_t install_err = i2c_driver_install(BOARD_DISPLAY_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if(install_err != ESP_OK && install_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(install_err);
    }

#if BOARD_PIN_DISPLAY_RST >= 0
    gpio_set_direction((gpio_num_t)BOARD_PIN_DISPLAY_RST, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)BOARD_PIN_DISPLAY_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)BOARD_PIN_DISPLAY_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
#endif

#if defined(BOARD_DISPLAY_SSD1306)
    /* SSD1306 128x64, page addressing mode. */
    static const uint8_t init_commands[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x02, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF,
    };
#else
    /* SH1106: internal DC/DC enabled, segment/common directions chosen for
       the conventional 128x64 module orientation. */
    static const uint8_t init_commands[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0xAD, 0x8B, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0x7F,
        0xD9, 0x22, 0xDB, 0x35, 0xA4, 0xA6, 0xAF,
    };
#endif
    ESP_ERROR_CHECK(sh1106_commands(init_commands, sizeof(init_commands)));
    display_ready = true;

    uint8_t clear[FB_SIZE] = {0};
    furi_hal_display_commit(clear, sizeof(clear));
}

void furi_hal_display_commit(const uint8_t* data, uint32_t size) {
    if(!display_ready || !data || size < FB_SIZE) return;

    uint8_t page_data[FB_WIDTH + 1U];
    page_data[0] = 0x40;

    for(uint8_t page = 0; page < 8U; page++) {
        /* SH1106 RAM is 132 columns wide; SSD1306 starts at column zero. */
#if defined(BOARD_DISPLAY_SSD1306)
        const uint8_t page_commands[] = {(uint8_t)(0xB0U | page), 0x00, 0x10};
#else
        const uint8_t page_commands[] = {(uint8_t)(0xB0U | page), 0x02, 0x10};
#endif
        if(sh1106_commands(page_commands, sizeof(page_commands)) != ESP_OK) return;
        memcpy(&page_data[1], &data[page * FB_WIDTH], FB_WIDTH);
        if(sh1106_write(page_data, sizeof(page_data)) != ESP_OK) return;
    }
}

/* Direct RGB565 drawing and the frame-diff cache of the colour LCD backend have no
 * counterpart on a 1-bpp OLED: commit() always sends the whole frame. They still
 * have to exist because the GUI (gui_direct_draw_release) and the Sub-GHz spectrum
 * view call them on every board; leaving them out broke linking this board. */
void furi_hal_display_blit(int x0, int y0, int x1, int y1, const void* data) {
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    (void)data;
}

void furi_hal_display_invalidate(void) {
}

void furi_hal_display_set_backlight(uint8_t brightness) {
    (void)brightness;
}

static bool panel_is_asleep = false;
static uint32_t panel_sleep_transition_count = 0;

void furi_hal_display_sleep(void) {
    if(!display_ready) return;
    furi_hal_power_allow_light_sleep(false);
    if(furi_hal_display_is_asleep()) return;
    const uint8_t command = 0xAE; /* display off */
    if(sh1106_commands(&command, 1) == ESP_OK) {
        FURI_CRITICAL_ENTER();
        if(!panel_is_asleep) {
            panel_is_asleep = true;
            panel_sleep_transition_count++;
        }
        FURI_CRITICAL_EXIT();
    }
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
    if(!display_ready) return;
    furi_hal_power_allow_light_sleep(false);
    if(!furi_hal_display_is_asleep()) return;
    const uint8_t command = 0xAF; /* display on */
    if(sh1106_commands(&command, 1) == ESP_OK) {
        FURI_CRITICAL_ENTER();
        if(panel_is_asleep) {
            panel_is_asleep = false;
            panel_sleep_transition_count++;
        }
        FuriHalDisplayWakeCallback callback = display_wake_callback;
        void* context = display_wake_context;
        FURI_CRITICAL_EXIT();
        if(callback) callback(context);
    }
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
    return FB_WIDTH;
}

uint16_t furi_hal_display_get_v_res(void) {
    return FB_HEIGHT;
}

esp_lcd_panel_handle_t furi_hal_display_get_panel_handle(void) {
    return NULL;
}

void furi_hal_display_set_fg_color(uint16_t color) {
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
