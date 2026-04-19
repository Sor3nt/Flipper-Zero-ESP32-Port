/**
 * @file furi_hal_light.c
 * Light control HAL — backlight PWM, board-driven configuration
 */

#include "furi_hal_light.h"
#include "furi_hal_resources.h"
#include "boards/board.h"

#include <driver/ledc.h>
#include <esp_log.h>

#define BACKLIGHT_LEDC_TIMER    LEDC_TIMER_0
#define BACKLIGHT_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BACKLIGHT_LEDC_SPEED    LEDC_LOW_SPEED_MODE
#define BACKLIGHT_LEDC_FREQ     5000
#define BACKLIGHT_LEDC_DUTY_RES LEDC_TIMER_8_BIT

static const char* TAG = "FuriHalLight";

static uint32_t furi_hal_light_backlight_duty_from_value(uint8_t value) {
#if BOARD_LCD_BL_ACTIVE_LOW
    /* Active-low backlight (e.g. P-channel high-side switch) */
    return UINT8_MAX - value;
#else
    return value;
#endif
}

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
}

void furi_hal_light_set(Light light, uint8_t value) {
    if(light & LightBacklight) {
        ledc_set_duty(
            BACKLIGHT_LEDC_SPEED,
            BACKLIGHT_LEDC_CHANNEL,
            furi_hal_light_backlight_duty_from_value(value));
        ledc_update_duty(BACKLIGHT_LEDC_SPEED, BACKLIGHT_LEDC_CHANNEL);
    }
    /* RGB LEDs: no hardware, ignore */
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
