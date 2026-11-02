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

#include <furi.h>
#include <driver/ledc.h>
#include <driver/gpio.h>
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
static bool light_shutdown_started = false;

#ifdef BOARD_PIN_WS2812_DATA
static led_strip_handle_t led_strip = NULL;
static FuriMutex* light_mutex = NULL;
/* Cached per-channel brightness (0-255). The Light enum lets callers update
 * one channel at a time (e.g. furi_hal_light_set(LightRed, 0xFF)); we cache
 * each channel and push the combined RGB to all WS2812 pixels on every
 * change. */
static uint8_t led_r = 0, led_g = 0, led_b = 0;
static bool ws2812_sleep_suspended = false;
/* Drive the strip through the RMT's DMA channel. Without DMA the RMT only holds
 * 48 symbols (one block) while a frame is 24 symbols per LED + reset, so the
 * driver has to refill the channel memory from an interrupt halfway through the
 * frame, and that refill has 24 symbols = 30 us to arrive. Any interrupt that
 * gets in the way (LCD DMA, SPI, GPIO, USB) makes the strip clock out garbage,
 * seen as flicker / wrong colours exactly while the Sub-GHz Spectrum and
 * Frequency Analyzer keep the SPI bus and the display busy. In DMA mode the
 * whole frame is encoded up front, so no mid-frame interrupt is needed. */
static bool ws2812_use_dma = true;
/* DMA buffer in symbols (two ping-pong halves). Must hold a whole frame in one
 * half: BOARD_WS2812_LED_COUNT * 24 + reset symbol, rounded up to 48. */
#define WS2812_DMA_MEM_BLOCK_SYMBOLS (2 * (((BOARD_WS2812_LED_COUNT * 24 + 1) + 47) / 48 * 48))

static void furi_hal_light_ws2812_init(void);

static void furi_hal_light_ws2812_set_pixels(uint8_t r, uint8_t g, uint8_t b) {
    if(!led_strip) return;
    for(uint16_t i = 0; i < BOARD_WS2812_LED_COUNT; i++) {
        led_strip_set_pixel(led_strip, i, r, g, b);
    }
}
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
static bool furi_hal_light_ws2812_refresh(void) {
    if(!led_strip) return false;
    esp_err_t err;
    err = led_strip_refresh(led_strip);
    if(err == ESP_OK) return true;

    /* A peripheral may have been torn down by another owner. Recreate the
     * driver once so shutdown can still transmit a valid all-black frame. */
    ESP_LOGW(TAG, "WS2812 refresh failed: %s; recreating RMT channel",
        esp_err_to_name(err));
    (void)led_strip_del(led_strip);
    led_strip = NULL;
    /* If DMA mode itself is what fails at run time, do not keep recreating it. */
    ws2812_use_dma = false;
    furi_hal_light_ws2812_init();
    if(!led_strip) return false;
    const uint8_t r = ws2812_sleep_suspended ? 0 : led_r;
    const uint8_t g = ws2812_sleep_suspended ? 0 : led_g;
    const uint8_t b = ws2812_sleep_suspended ? 0 : led_b;
    furi_hal_light_ws2812_set_pixels(r, g, b);
    err = led_strip_refresh(led_strip);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 refresh retry failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void furi_hal_light_ws2812_push(void) {
    if(!led_strip || ws2812_sleep_suspended) return;
    furi_hal_light_ws2812_set_pixels(led_r, led_g, led_b);
    (void)furi_hal_light_ws2812_refresh();
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
        .mem_block_symbols = ws2812_use_dma ? WS2812_DMA_MEM_BLOCK_SYMBOLS : 0,
        .flags = { .with_dma = ws2812_use_dma },
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if(err != ESP_OK && ws2812_use_dma) {
        /* No free RMT DMA channel / GDMA / DMA-capable memory: stay functional on
         * the interrupt-fed channel instead of losing the LEDs. */
        ESP_LOGW(TAG, "WS2812 RMT DMA unavailable (%s); using interrupt-fed RMT",
            esp_err_to_name(err));
        ws2812_use_dma = false;
        rmt_config.mem_block_symbols = 0;
        rmt_config.flags.with_dma = false;
        err = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    }
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "WS2812 RMT init failed: %s", esp_err_to_name(err));
        led_strip = NULL;
        return;
    }
    led_strip_clear(led_strip);
    /* Strip starts dark; notification service will push the saved
     * settings.led_color once it's loaded the user's preferences. */
    ESP_LOGI(TAG, "WS2812 ring (%d LEDs) on GPIO%d initialized (%s)",
        BOARD_WS2812_LED_COUNT, BOARD_PIN_WS2812_DATA,
        ws2812_use_dma ? "RMT DMA" : "RMT interrupt-fed");
}
#endif

void furi_hal_light_init(void) {
    __atomic_store_n(&light_shutdown_started, false, __ATOMIC_RELEASE);
#ifdef BOARD_PIN_WS2812_DATA
    ws2812_sleep_suspended = false;
    if(!light_mutex) {
        light_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    }
#endif
    /* Release any deep-sleep GPIO hold on the backlight pin. The power-off path
     * latches this pin LOW with gpio_hold so the backlight stays off in deep
     * sleep; the latch survives the deep-sleep wake reset, so we must clear it
     * here before LEDC can drive the pin again. No-op on a cold boot. */
    
    int bl_pin = gpio_lcd_bl.pin;
    if (bl_pin >= GPIO_NUM_MAX) {
        ESP_LOGW(TAG, "Backlight pin not valid (gpio=%d), skipping LEDC init", bl_pin);
    } else {
    gpio_hold_dis((gpio_num_t)gpio_lcd_bl.pin);

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
    
#ifdef BOARD_PIN_WS2812_DATA
    furi_hal_light_ws2812_init();
#endif
}

void furi_hal_light_set(Light light, uint8_t value) {
    if(__atomic_load_n(&light_shutdown_started, __ATOMIC_ACQUIRE)) return;
    if(light & LightBacklight) {
        ledc_set_duty(
            BACKLIGHT_LEDC_SPEED,
            BACKLIGHT_LEDC_CHANNEL,
            furi_hal_light_backlight_duty_from_value(value));
        ledc_update_duty(BACKLIGHT_LEDC_SPEED, BACKLIGHT_LEDC_CHANNEL);
    }
#ifdef BOARD_PIN_WS2812_DATA
    const bool rgb_changed = (light & (LightRed | LightGreen | LightBlue)) != 0;
    if(rgb_changed && light_mutex &&
       furi_mutex_acquire(light_mutex, FuriWaitForever) == FuriStatusOk) {
        if(!__atomic_load_n(&light_shutdown_started, __ATOMIC_ACQUIRE)) {
            if(light & LightRed) led_r = value;
            if(light & LightGreen) led_g = value;
            if(light & LightBlue) led_b = value;
            furi_hal_light_ws2812_push();
        }
        furi_mutex_release(light_mutex);
    }
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
    if(!light_mutex ||
       furi_mutex_acquire(light_mutex, FuriWaitForever) != FuriStatusOk) {
        return;
    }
    if(__atomic_load_n(&light_shutdown_started, __ATOMIC_ACQUIRE)) {
        furi_mutex_release(light_mutex);
        return;
    }
    led_r = r; led_g = g; led_b = b;
    furi_hal_light_ws2812_push();
    furi_mutex_release(light_mutex);
}

bool furi_hal_light_shutdown_ws2812(void) {
    if(!light_mutex ||
       furi_mutex_acquire(light_mutex, FuriWaitForever) != FuriStatusOk) {
        return false;
    }
    if(__atomic_load_n(&light_shutdown_started, __ATOMIC_ACQUIRE)) {
        furi_mutex_release(light_mutex);
        return true;
    }

    __atomic_store_n(&light_shutdown_started, true, __ATOMIC_RELEASE);
    led_r = 0;
    led_g = 0;
    led_b = 0;
    bool sent = false;
    if(led_strip) {
        for(uint16_t i = 0; i < BOARD_WS2812_LED_COUNT; i++) {
            led_strip_set_pixel(led_strip, i, 0, 0, 0);
        }
        sent = furi_hal_light_ws2812_refresh();
    }
    furi_mutex_release(light_mutex);
    return sent;
}

void furi_hal_light_set_ws2812_sleep(bool sleeping) {
    if(!light_mutex ||
       furi_mutex_acquire(light_mutex, FuriWaitForever) != FuriStatusOk) {
        return;
    }
    if(__atomic_load_n(&light_shutdown_started, __ATOMIC_ACQUIRE) ||
       ws2812_sleep_suspended == sleeping) {
        furi_mutex_release(light_mutex);
        return;
    }

    ws2812_sleep_suspended = sleeping;
    if(sleeping) {
        furi_hal_light_ws2812_set_pixels(0, 0, 0);
        if(!furi_hal_light_ws2812_refresh()) {
            ESP_LOGW(TAG, "Could not confirm WS2812 sleep-off frame");
        }
        const esp_err_t pull_err =
            gpio_set_pull_mode((gpio_num_t)BOARD_PIN_WS2812_DATA, GPIO_PULLDOWN_ONLY);
        if(pull_err != ESP_OK) {
            ESP_LOGW(TAG, "Could not enable WS2812 data pull-down: %s", esp_err_to_name(pull_err));
        }
    } else {
        const esp_err_t pull_err =
            gpio_set_pull_mode((gpio_num_t)BOARD_PIN_WS2812_DATA, GPIO_FLOATING);
        if(pull_err != ESP_OK) {
            ESP_LOGW(TAG, "Could not clear WS2812 data pull-down: %s", esp_err_to_name(pull_err));
        }
        furi_hal_light_ws2812_push();
    }
    furi_mutex_release(light_mutex);
}

/* Staging and pushing take the same mutex as every other WS2812 access. Without it a
 * refresh could still send a coloured frame after furi_hal_light_shutdown_ws2812()
 * had sent the final black one (LEDs lit through deep sleep), and led_strip could be
 * deleted and recreated by a failed refresh while this code still used the old handle. */
void furi_hal_light_set_pixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {
    if(idx >= BOARD_WS2812_LED_COUNT) return;
    if(!light_mutex || furi_mutex_acquire(light_mutex, FuriWaitForever) != FuriStatusOk) return;
    if(!__atomic_load_n(&light_shutdown_started, __ATOMIC_ACQUIRE) &&
       !ws2812_sleep_suspended && led_strip) {
        led_strip_set_pixel(led_strip, idx, r, g, b);
    }
    furi_mutex_release(light_mutex);
}

void furi_hal_light_refresh(void) {
    if(!light_mutex || furi_mutex_acquire(light_mutex, FuriWaitForever) != FuriStatusOk) return;
    if(!__atomic_load_n(&light_shutdown_started, __ATOMIC_ACQUIRE) &&
       !ws2812_sleep_suspended && led_strip) {
        led_strip_refresh(led_strip);
    }
    furi_mutex_release(light_mutex);
}

uint16_t furi_hal_light_pixel_count(void) {
    return BOARD_WS2812_LED_COUNT;
}
#else
void furi_hal_light_set_rgb_all(uint8_t r, uint8_t g, uint8_t b) {
    (void)r; (void)g; (void)b;
}
bool furi_hal_light_shutdown_ws2812(void) {
    return true;
}
void furi_hal_light_set_ws2812_sleep(bool sleeping) {
    (void)sleeping;
}
void furi_hal_light_set_pixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {
    (void)idx; (void)r; (void)g; (void)b;
}
void furi_hal_light_refresh(void) {}
uint16_t furi_hal_light_pixel_count(void) { return 0; }
#endif
