/**
 * @file furi_hal_light.c
 * Light control HAL — LCD backlight (LEDC PWM) + WS2812 RGB ring (RMT).
 *
 * The T-Embed CC1101 PLUS has two distinct light hardware:
 *   - Single-channel white LCD backlight on BOARD_PIN_LCD_BL (LEDC PWM)
 *   - 8x WS2812 RGB LED ring on BOARD_PIN_WS2812_DATA (driven via RMT,
 *     power-gated by BOARD_PIN_PWR_EN which is asserted in furi_hal_init_early)
 *
 * The RGB ring is treated as one logical RGB LED — all 8 LEDs mirror the
 * same color. This matches the Light enum (Red/Green/Blue bit flags) and
 * mirrors how Bruce drives the same hardware.
 */

#include "furi_hal_light.h"
#include "furi_hal_resources.h"
#include "boards/board.h"

#include <driver/ledc.h>
#include <esp_log.h>

#ifdef BOARD_PIN_WS2812_DATA
#include <led_strip.h>
#endif

#define BACKLIGHT_LEDC_TIMER    LEDC_TIMER_0
#define BACKLIGHT_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BACKLIGHT_LEDC_SPEED    LEDC_LOW_SPEED_MODE
#define BACKLIGHT_LEDC_FREQ     5000
#define BACKLIGHT_LEDC_DUTY_RES LEDC_TIMER_8_BIT

static const char* TAG = "FuriHalLight";

#ifdef BOARD_PIN_WS2812_DATA
static led_strip_handle_t led_strip = NULL;
/* Cached per-channel brightness (0-255). The Light enum lets callers update
 * one channel at a time (e.g. furi_hal_light_set(LightRed, 0xFF)); we cache
 * each channel and push the combined RGB to all WS2812 pixels on every
 * change. */
static uint8_t led_r = 0, led_g = 0, led_b = 0;
#endif

static uint32_t furi_hal_light_backlight_duty_from_value(uint8_t value) {
#if BOARD_LCD_BL_ACTIVE_LOW
    /* Active-low backlight (e.g. P-channel high-side switch) */
    return UINT8_MAX - value;
#else
    return value;
#endif
}

#ifdef BOARD_PIN_WS2812_DATA
static void furi_hal_light_ws2812_push(void) {
    if(!led_strip) return;
    for(uint16_t i = 0; i < BOARD_WS2812_LED_COUNT; i++) {
        led_strip_set_pixel(led_strip, i, led_r, led_g, led_b);
    }
    led_strip_refresh(led_strip);
}

static void furi_hal_light_ws2812_init(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = BOARD_PIN_WS2812_DATA,
        .max_leds = BOARD_WS2812_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  /* 10 MHz */
        .mem_block_symbols = 0,             /* default */
        .flags = { .with_dma = false },
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "WS2812 RMT init failed: %s", esp_err_to_name(err));
        led_strip = NULL;
        return;
    }
    led_strip_clear(led_strip);
    /* Strip starts dark; notification service will push the saved
     * settings.led_color once it's loaded the user's preferences. */
    ESP_LOGI(TAG, "WS2812 ring (%d LEDs) on GPIO%d initialized",
        BOARD_WS2812_LED_COUNT, BOARD_PIN_WS2812_DATA);
}
#endif

void furi_hal_light_init(void) {
    /* Configure LEDC timer for backlight PWM */
    ledc_timer_config_t timer_conf = {
        .speed_mode = BACKLIGHT_LEDC_SPEED,
        .duty_resolution = BACKLIGHT_LEDC_DUTY_RES,
        .timer_num = BACKLIGHT_LEDC_TIMER,
        .freq_hz = BACKLIGHT_LEDC_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

    /* Configure LEDC channel on backlight pin */
    ledc_channel_config_t channel_conf = {
        .speed_mode = BACKLIGHT_LEDC_SPEED,
        .channel = BACKLIGHT_LEDC_CHANNEL,
        .timer_sel = BACKLIGHT_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = gpio_lcd_bl.pin,
        .duty = furi_hal_light_backlight_duty_from_value(255),
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_conf));

    ESP_LOGI(TAG, "Backlight PWM initialized on GPIO%d", gpio_lcd_bl.pin);

#ifdef BOARD_PIN_WS2812_DATA
    furi_hal_light_ws2812_init();
#endif
}

void furi_hal_light_set(Light light, uint8_t value) {
    if(light & LightBacklight) {
        ledc_set_duty(
            BACKLIGHT_LEDC_SPEED,
            BACKLIGHT_LEDC_CHANNEL,
            furi_hal_light_backlight_duty_from_value(value));
        ledc_update_duty(BACKLIGHT_LEDC_SPEED, BACKLIGHT_LEDC_CHANNEL);
    }
#ifdef BOARD_PIN_WS2812_DATA
    bool rgb_changed = false;
    if(light & LightRed)   { led_r = value; rgb_changed = true; }
    if(light & LightGreen) { led_g = value; rgb_changed = true; }
    if(light & LightBlue)  { led_b = value; rgb_changed = true; }
    if(rgb_changed) furi_hal_light_ws2812_push();
#endif
}

void furi_hal_light_blink_start(Light light, uint8_t brightness, uint16_t on_time, uint16_t period) {
    (void)light;
    (void)brightness;
    (void)on_time;
    (void)period;
}

void furi_hal_light_blink_stop(void) {
}

void furi_hal_light_blink_set_color(Light light) {
    (void)light;
}

void furi_hal_light_sequence(const char* sequence) {
    (void)sequence;
}

#ifdef BOARD_PIN_WS2812_DATA
void furi_hal_light_set_rgb_all(uint8_t r, uint8_t g, uint8_t b) {
    led_r = r; led_g = g; led_b = b;
    if(!led_strip) return;
    for(uint16_t i = 0; i < BOARD_WS2812_LED_COUNT; i++) {
        led_strip_set_pixel(led_strip, i, r, g, b);
    }
    led_strip_refresh(led_strip);
}

void furi_hal_light_set_pixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {
    if(!led_strip || idx >= BOARD_WS2812_LED_COUNT) return;
    led_strip_set_pixel(led_strip, idx, r, g, b);
}

void furi_hal_light_refresh(void) {
    if(!led_strip) return;
    led_strip_refresh(led_strip);
}

uint16_t furi_hal_light_pixel_count(void) {
    return BOARD_WS2812_LED_COUNT;
}
#else
void furi_hal_light_set_rgb_all(uint8_t r, uint8_t g, uint8_t b) {
    (void)r; (void)g; (void)b;
}
void furi_hal_light_set_pixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {
    (void)idx; (void)r; (void)g; (void)b;
}
void furi_hal_light_refresh(void) {}
uint16_t furi_hal_light_pixel_count(void) { return 0; }
#endif
