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
#include <driver/gpio.h>
#include <esp_log.h>

#ifdef BOARD_PIN_WS2812_DATA
#include <led_strip.h>
#include <esp_rom_gpio.h>
#include <soc/gpio_sig_map.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
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
/* Serializes every WS2812 access: the notification effect timer, the input
 * thread, the settings app and FAPs all write the ring. While suspended (screen
 * off) or shut down (power-off) the hardware is left dark and only the cache
 * above follows the writers. */
static SemaphoreHandle_t light_mutex = NULL;
static StaticSemaphore_t light_mutex_buffer;
static uint8_t ws2812_suspended = 0; /* FuriHalLightSuspend bits */
static bool ws2812_shutdown = false;
static FuriHalLightSuspendCallback ws2812_suspend_callback = NULL;
static void* ws2812_suspend_context = NULL;
/* DMA avoids a refill interrupt in the middle of each WS2812 frame. Keep a
 * complete frame in each RMT DMA half; the channel buffer is split in two. */
static bool ws2812_use_dma = true;
#define WS2812_RMT_SYMBOLS_PER_PIXEL 24U
#define WS2812_RMT_SYMBOLS_PER_BLOCK 48U
#define WS2812_DMA_MEM_BLOCK_SYMBOLS \
    (2U * (((BOARD_WS2812_LED_COUNT * WS2812_RMT_SYMBOLS_PER_PIXEL + 1U + \
             WS2812_RMT_SYMBOLS_PER_BLOCK - 1U) / WS2812_RMT_SYMBOLS_PER_BLOCK) * \
           WS2812_RMT_SYMBOLS_PER_BLOCK))

static void furi_hal_light_lock(void) {
    if(light_mutex) xSemaphoreTake(light_mutex, portMAX_DELAY);
}

static void furi_hal_light_unlock(void) {
    if(light_mutex) xSemaphoreGive(light_mutex);
}

static bool furi_hal_light_ws2812_writable(void) {
    return led_strip && !ws2812_suspended && !ws2812_shutdown;
}

/* Caller holds light_mutex. */
static esp_err_t furi_hal_light_ws2812_set_pixels(uint8_t r, uint8_t g, uint8_t b) {
    if(!led_strip) return ESP_ERR_INVALID_STATE;
    for(uint16_t i = 0; i < BOARD_WS2812_LED_COUNT; i++) {
        const esp_err_t err = led_strip_set_pixel(led_strip, i, r, g, b);
        if(err != ESP_OK) return err;
    }
    return ESP_OK;
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
static void furi_hal_light_ws2812_init(void);

/* Caller holds light_mutex. Recover once from a failed transmission, falling
 * back to interrupt-fed RMT if DMA failed at runtime. */
static bool furi_hal_light_ws2812_refresh(void) {
    if(!led_strip) return false;

    esp_err_t err = led_strip_refresh(led_strip);
    if(err == ESP_OK) return true;

    ESP_LOGW(TAG, "WS2812 refresh failed: %s; recreating RMT channel",
        esp_err_to_name(err));
    const esp_err_t del_err = led_strip_del(led_strip);
    if(del_err != ESP_OK) {
        ESP_LOGE(TAG, "WS2812 RMT channel recovery failed: %s", esp_err_to_name(del_err));
        return false;
    }

    led_strip = NULL;
    ws2812_use_dma = false;
    furi_hal_light_ws2812_init();
    if(!led_strip) return false;

    const uint8_t r = ws2812_suspended ? 0 : led_r;
    const uint8_t g = ws2812_suspended ? 0 : led_g;
    const uint8_t b = ws2812_suspended ? 0 : led_b;
    err = furi_hal_light_ws2812_set_pixels(r, g, b);
    if(err == ESP_OK) err = led_strip_refresh(led_strip);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 refresh retry failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* Caller holds light_mutex. */
static void furi_hal_light_ws2812_push(void) {
    if(!furi_hal_light_ws2812_writable()) return;
    const esp_err_t err = furi_hal_light_ws2812_set_pixels(led_r, led_g, led_b);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 pixel update failed: %s", esp_err_to_name(err));
        return;
    }
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
    err = led_strip_clear(led_strip);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 initial clear failed: %s", esp_err_to_name(err));
    }
    /* Strip starts dark; notification service will push the saved
     * settings.led_color once it's loaded the user's preferences. */
    ESP_LOGI(TAG, "WS2812 ring (%d LEDs) on GPIO%d initialized (%s)",
        BOARD_WS2812_LED_COUNT, BOARD_PIN_WS2812_DATA,
        ws2812_use_dma ? "RMT DMA" : "RMT interrupt-fed");
}
#endif

void furi_hal_light_init(void) {
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
    if(!light_mutex) light_mutex = xSemaphoreCreateMutexStatic(&light_mutex_buffer);
    furi_hal_light_ws2812_init();
#endif
}

static uint8_t s_backlight_value = 255; /* last LightBacklight value; read by furi_hal_light_get_backlight */

void furi_hal_light_set(Light light, uint8_t value) {
    if(light & LightBacklight) {
        s_backlight_value = value;
        ledc_set_duty(
            BACKLIGHT_LEDC_SPEED,
            BACKLIGHT_LEDC_CHANNEL,
            furi_hal_light_backlight_duty_from_value(value));
        ledc_update_duty(BACKLIGHT_LEDC_SPEED, BACKLIGHT_LEDC_CHANNEL);
    }
#ifdef BOARD_PIN_WS2812_DATA
    bool rgb_changed = false;
    if(light & (LightRed | LightGreen | LightBlue)) rgb_changed = true;
    if(rgb_changed) {
        furi_hal_light_lock();
        if(!ws2812_shutdown) {
            if(light & LightRed) led_r = value;
            if(light & LightGreen) led_g = value;
            if(light & LightBlue) led_b = value;
            furi_hal_light_ws2812_push();
        }
        furi_hal_light_unlock();
    }
#endif
}

uint8_t furi_hal_light_get_backlight(void) {
    return s_backlight_value;
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
    furi_hal_light_lock();
    led_r = r; led_g = g; led_b = b;
    furi_hal_light_ws2812_push();
    furi_hal_light_unlock();
}

void furi_hal_light_set_pixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {
    if(idx >= BOARD_WS2812_LED_COUNT) return;
    furi_hal_light_lock();
    if(furi_hal_light_ws2812_writable()) {
        const esp_err_t err = led_strip_set_pixel(led_strip, idx, r, g, b);
        if(err != ESP_OK) {
            ESP_LOGW(TAG, "WS2812 pixel %u update failed: %s", (unsigned)idx, esp_err_to_name(err));
        }
    }
    furi_hal_light_unlock();
}

void furi_hal_light_refresh(void) {
    furi_hal_light_lock();
    if(furi_hal_light_ws2812_writable()) (void)furi_hal_light_ws2812_refresh();
    furi_hal_light_unlock();
}

void furi_hal_light_set_ws2812_suspended(FuriHalLightSuspend reason, bool suspended) {
    FuriHalLightSuspendCallback callback = NULL;
    void* context = NULL;
    bool now_dark = false;

    furi_hal_light_lock();
    const uint8_t before = ws2812_suspended;
    if(suspended) {
        ws2812_suspended |= (uint8_t)reason;
    } else {
        ws2812_suspended &= (uint8_t)~reason;
    }
    if(!ws2812_shutdown) {
        if(before == 0 && ws2812_suspended != 0) {
            /* First reason: black frame, then every writer is blocked. */
            if(led_strip) led_strip_clear(led_strip);
            callback = ws2812_suspend_callback;
            context = ws2812_suspend_context;
            now_dark = true;
        } else if(before != 0 && ws2812_suspended == 0) {
            callback = ws2812_suspend_callback;
            context = ws2812_suspend_context;
            if(!callback) furi_hal_light_ws2812_push();
        }
    }
    furi_hal_light_unlock();

    if(callback) callback(context, now_dark);
}

bool furi_hal_light_is_ws2812_suspended(void) {
    furi_hal_light_lock();
    const bool dark = ws2812_suspended != 0 || ws2812_shutdown;
    furi_hal_light_unlock();
    return dark;
}

void furi_hal_light_set_ws2812_suspend_callback(FuriHalLightSuspendCallback callback, void* context) {
    furi_hal_light_lock();
    ws2812_suspend_callback = callback;
    ws2812_suspend_context = context;
    furi_hal_light_unlock();
}

bool furi_hal_light_shutdown_ws2812(void) {
    furi_hal_light_lock();
    bool sent = true;
    if(led_strip && !ws2812_shutdown) {
        /* Synchronous: led_strip_refresh waits for rmt_tx_wait_all_done, so
         * the black frame is fully clocked out before we take the pad away. */
        sent = led_strip_clear(led_strip) == ESP_OK;
    }
    if(!ws2812_shutdown) {
        /* Detach the data pin from the RMT peripheral and drive it LOW as a
         * plain GPIO, so its level no longer depends on RMT idle state and is
         * well defined when the deep-sleep pin table latches it. Done under
         * light_mutex with ws2812_shutdown set, so no writer can re-attach RMT
         * or relight the ring afterwards (every WS2812 call takes this lock and
         * bails on ws2812_shutdown). */
        const gpio_num_t pin = (gpio_num_t)BOARD_PIN_WS2812_DATA;
        gpio_set_level(pin, 0);
        esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_pull_mode(pin, GPIO_FLOATING);
    }
    ws2812_shutdown = true;
    furi_hal_light_unlock();
    return sent;
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
void furi_hal_light_set_ws2812_suspended(FuriHalLightSuspend reason, bool suspended) {
    (void)reason;
    (void)suspended;
}
bool furi_hal_light_is_ws2812_suspended(void) {
    return false;
}
void furi_hal_light_set_ws2812_suspend_callback(FuriHalLightSuspendCallback callback, void* context) {
    (void)callback;
    (void)context;
}
bool furi_hal_light_shutdown_ws2812(void) {
    return true;
}
#endif
