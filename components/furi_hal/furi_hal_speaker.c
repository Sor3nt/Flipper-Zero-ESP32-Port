#include "furi_hal_speaker.h"
#include "furi_hal_power.h"
#include "boards/board.h"
#include <furi.h>

#if BOARD_HAS_SPEAKER || BOARD_HAS_BUZZER
static inline float speaker_volume_curve(float v) {
    if(v < 0.0f) v = 0.0f;
    if(v > 1.0f) v = 1.0f;
    return v * v * v;
}
#endif

#define TAG "FuriHalSpeaker"

#if BOARD_HAS_SPEAKER

#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SPEAKER_SAMPLE_RATE   44100
#define SPEAKER_BITS          16
#define SPEAKER_DMA_DESC_NUM  4
#define SPEAKER_DMA_FRAME_NUM 256
#define SPEAKER_THREAD_STACK  2048

#define SPEAKER_MAX_CYCLE_SAMPLES 1024

#define MIRROR_SAMPLE_RATE 16000
#define MIRROR_BUF_FRAMES  64

typedef enum {
    SpeakerModeIdle,
    SpeakerModeTone,
    SpeakerModeGdoMirror,
} SpeakerMode;

static FuriMutex* speaker_mutex = NULL;
static i2s_chan_handle_t i2s_tx_handle = NULL;
static bool i2s_channel_enabled = false;

static volatile SpeakerMode speaker_mode = SpeakerModeIdle;

static volatile float speaker_frequency = 0.0f;
static volatile float speaker_volume = 0.0f;
static volatile bool speaker_buffer_dirty = true;

static volatile gpio_num_t speaker_mirror_pin = (gpio_num_t)-1;
static volatile float speaker_mirror_volume = 0.0f;

static FuriThread* speaker_thread = NULL;
static volatile bool speaker_thread_run = false;

static int16_t* wave_buffer = NULL;
static size_t wave_buffer_samples = 0;
static size_t wave_buffer_bytes = 0;
static bool speaker_light_sleep_inhibited = false;

static void speaker_set_light_sleep_inhibited(bool inhibited) {
    if(speaker_light_sleep_inhibited == inhibited) return;
    speaker_light_sleep_inhibited = inhibited;
    if(inhibited) {
        furi_hal_power_light_sleep_inhibit_enter();
    } else {
        furi_hal_power_light_sleep_inhibit_exit();
    }
}

static void speaker_generate_buffer(void) {
    /* Clear the flag BEFORE reading the parameters: a start()/set_volume() that lands
     * while the samples are being computed sets it again, so the next loop pass
     * regenerates. Clearing it at the end would swallow that change and keep playing
     * the old tone. */
    speaker_buffer_dirty = false;

    float freq = speaker_frequency;
    float vol = speaker_volume_curve(speaker_volume);

    if(freq < 1.0f) freq = 1.0f;

    uint32_t samples_per_cycle = (uint32_t)(SPEAKER_SAMPLE_RATE / freq);
    if(samples_per_cycle < 2) samples_per_cycle = 2;
    if(samples_per_cycle > SPEAKER_MAX_CYCLE_SAMPLES) samples_per_cycle = SPEAKER_MAX_CYCLE_SAMPLES;

    size_t needed = samples_per_cycle * 2 * sizeof(int16_t);
    if(wave_buffer == NULL || wave_buffer_samples != samples_per_cycle) {
        if(wave_buffer) free(wave_buffer);
        wave_buffer = malloc(needed);
        wave_buffer_samples = wave_buffer ? samples_per_cycle : 0;
    }
    if(!wave_buffer) {
        /* Out of memory: stay silent instead of writing through a NULL pointer. */
        wave_buffer_bytes = 0;
        return;
    }
    wave_buffer_bytes = needed;

    float amplitude = vol * 32767.0f;
    for(uint32_t i = 0; i < samples_per_cycle; i++) {
        int16_t sample = (int16_t)(amplitude * sinf(2.0f * (float)M_PI * (float)i / (float)samples_per_cycle));
        wave_buffer[i * 2] = sample;
        wave_buffer[i * 2 + 1] = sample;
    }
}

static int32_t speaker_writer_thread(void* context) {
    UNUSED(context);

    static int16_t mirror_buf[MIRROR_BUF_FRAMES * 2];

    while(speaker_thread_run) {
        SpeakerMode mode = speaker_mode;

        if(mode == SpeakerModeTone) {
            if(speaker_buffer_dirty) {
                speaker_generate_buffer();
            }
            if(wave_buffer && wave_buffer_bytes > 0) {
                size_t bytes_written = 0;
                i2s_channel_write(i2s_tx_handle, wave_buffer, wave_buffer_bytes, &bytes_written, 100);
            } else {
                furi_delay_ms(5); /* nothing to play (allocation failed): do not spin */
            }
            continue;
        }

        if(mode == SpeakerModeGdoMirror) {
            const gpio_num_t pin = speaker_mirror_pin;
            const float vol = speaker_volume_curve(speaker_mirror_volume);
            const int16_t amp = (int16_t)(vol * 32767.0f);
            const int64_t period_us = 1000000 / MIRROR_SAMPLE_RATE;

            int64_t next_us = esp_timer_get_time();
            for(int i = 0; i < MIRROR_BUF_FRAMES; i++) {
                while(esp_timer_get_time() < next_us) { }
                next_us += period_us;
                int16_t s = gpio_get_level(pin) ? amp : (int16_t)-amp;
                mirror_buf[i * 2] = s;
                mirror_buf[i * 2 + 1] = s;
            }

            size_t bytes_written = 0;
            i2s_channel_write(i2s_tx_handle, mirror_buf, sizeof(mirror_buf), &bytes_written, 100);
            continue;
        }

        furi_delay_ms(5);
    }

    return 0;
}

void furi_hal_speaker_init(void) {
    furi_assert(speaker_mutex == NULL);
    speaker_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    gpio_reset_pin((gpio_num_t)BOARD_PIN_SPEAKER_WCLK);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = SPEAKER_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = SPEAKER_DMA_FRAME_NUM;
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPEAKER_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)BOARD_PIN_SPEAKER_BCLK,
            .ws = (gpio_num_t)BOARD_PIN_SPEAKER_WCLK,
            .dout = (gpio_num_t)BOARD_PIN_SPEAKER_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_handle, &std_cfg));

    speaker_thread = furi_thread_alloc_ex("SpeakerWorker", SPEAKER_THREAD_STACK, speaker_writer_thread, NULL);
    speaker_thread_run = true;
    furi_thread_start(speaker_thread);

    FURI_LOG_I(TAG, "Init OK (I2S, BCLK=%d WS=%d DOUT=%d)",
               BOARD_PIN_SPEAKER_BCLK, BOARD_PIN_SPEAKER_WCLK, BOARD_PIN_SPEAKER_DOUT);
}

void furi_hal_speaker_deinit(void) {
    furi_check(speaker_mutex != NULL);

    speaker_set_light_sleep_inhibited(false);
    speaker_thread_run = false;
    furi_thread_join(speaker_thread);
    furi_thread_free(speaker_thread);
    speaker_thread = NULL;

    if(i2s_channel_enabled) {
        i2s_channel_disable(i2s_tx_handle);
        i2s_channel_enabled = false;
    }
    i2s_del_channel(i2s_tx_handle);
    i2s_tx_handle = NULL;

    if(wave_buffer) {
        free(wave_buffer);
        wave_buffer = NULL;
    }

    FuriMutex* mutex = speaker_mutex;
    speaker_mutex = NULL; /* unpublish first: is_mine()/acquire() check it lock-free */
    furi_mutex_free(mutex);
}

bool furi_hal_speaker_acquire(uint32_t timeout) {
    furi_check(!FURI_IS_IRQ_MODE());

    FuriMutex* mutex = speaker_mutex;
    if(!mutex) return false; /* HAL released by an app, see furi_hal_speaker_deinit() */
    if(furi_mutex_acquire(mutex, timeout) == FuriStatusOk) {
        if(!i2s_channel_enabled) {
            ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_handle));
            i2s_channel_enabled = true;
        }
        return true;
    }
    return false;
}

void furi_hal_speaker_release(void) {
    furi_check(!FURI_IS_IRQ_MODE());
    furi_check(furi_hal_speaker_is_mine());

    furi_hal_speaker_stop();
    furi_check(furi_mutex_release(speaker_mutex) == FuriStatusOk);
}

bool furi_hal_speaker_is_mine(void) {
    if(FURI_IS_IRQ_MODE()) return true;
    /* An app (Doom, Wolf3D, ...) may have taken the speaker over via
     * furi_hal_speaker_deinit(): nobody owns it then. Notification still asks. */
    FuriMutex* mutex = speaker_mutex;
    return mutex && (furi_mutex_get_owner(mutex) == furi_thread_get_current_id());
}

void furi_hal_speaker_start(float frequency, float volume) {
    furi_check(furi_hal_speaker_is_mine());

    if(volume <= 0.0f) {
        furi_hal_speaker_stop();
        return;
    }

    speaker_frequency = frequency;
    speaker_volume = volume;
    speaker_buffer_dirty = true;
    speaker_set_light_sleep_inhibited(true);
    speaker_mode = SpeakerModeTone;
}

void furi_hal_speaker_start_gdo_mirror(const GpioPin* gdo_pin, float volume) {
    furi_check(furi_hal_speaker_is_mine());
    furi_check(gdo_pin);
    furi_check(gdo_pin->pin < GPIO_NUM_MAX);

    if(volume <= 0.0f) {
        furi_hal_speaker_stop();
        return;
    }

    speaker_mirror_pin = (gpio_num_t)gdo_pin->pin;
    speaker_mirror_volume = volume;
    speaker_set_light_sleep_inhibited(true);
    speaker_mode = SpeakerModeGdoMirror;
}

void furi_hal_speaker_set_volume(float volume) {
    furi_check(furi_hal_speaker_is_mine());

    if(volume <= 0.0f) {
        furi_hal_speaker_stop();
        return;
    }

    speaker_volume = volume;
    speaker_mirror_volume = volume;
    speaker_buffer_dirty = true;
}

void furi_hal_speaker_stop(void) {
    furi_check(furi_hal_speaker_is_mine());
    speaker_mode = SpeakerModeIdle;
    speaker_set_light_sleep_inhibited(false);
}

bool furi_hal_speaker_is_playing(void) {
    return speaker_mode != SpeakerModeIdle;
}

#elif BOARD_HAS_BUZZER

#include <driver/ledc.h>
#include <soc/soc.h>

#define BUZZER_LEDC_TIMER    LEDC_TIMER_1
#define BUZZER_LEDC_CHANNEL  LEDC_CHANNEL_1
#define BUZZER_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BUZZER_DUTY_RES      LEDC_TIMER_14_BIT
/* LEDC_AUTO_CLK picks the 80 MHz APB clock first, which every tone here fits. */
#define BUZZER_LEDC_SRC_HZ   APB_CLK_FREQ

static FuriMutex* speaker_mutex = NULL;
static bool buzzer_ledc_ready = false;
static uint32_t buzzer_duty_res = BUZZER_DUTY_RES;
static volatile bool buzzer_is_playing = false;
static bool buzzer_light_sleep_inhibited = false;

static void buzzer_set_light_sleep_inhibited(bool inhibited) {
    if(buzzer_light_sleep_inhibited == inhibited) return;
    buzzer_light_sleep_inhibited = inhibited;
    if(inhibited) {
        furi_hal_power_light_sleep_inhibit_enter();
    } else {
        furi_hal_power_light_sleep_inhibit_exit();
    }
}

static uint32_t buzzer_duty_for_volume(float volume) {
    const uint32_t max_duty = (1u << buzzer_duty_res) - 1;
    return (uint32_t)(max_duty * 0.5f * speaker_volume_curve(volume));
}

/* A fixed 14-bit timer tops out at 80 MHz / 2^14 = 4.88 kHz, so higher notes
 * failed. Use the finest duty resolution that still reaches the requested tone;
 * the timer is only reconfigured when that resolution changes. */
static esp_err_t buzzer_set_frequency(uint32_t frequency) {
    const uint32_t duty_res = ledc_find_suitable_duty_resolution(BUZZER_LEDC_SRC_HZ, frequency);
    if(duty_res == 0) return ESP_ERR_INVALID_ARG;
    if(duty_res == buzzer_duty_res) {
        return ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, frequency);
    }

    ledc_timer_config_t timer_cfg = {
        .speed_mode = BUZZER_LEDC_MODE,
        .timer_num = BUZZER_LEDC_TIMER,
        .duty_resolution = duty_res,
        .freq_hz = frequency,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const esp_err_t err = ledc_timer_config(&timer_cfg);
    if(err == ESP_OK) buzzer_duty_res = duty_res;
    return err;
}

void furi_hal_speaker_init(void) {
    furi_assert(speaker_mutex == NULL);
    speaker_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    ledc_timer_config_t timer_cfg = {
        .speed_mode = BUZZER_LEDC_MODE,
        .timer_num = BUZZER_LEDC_TIMER,
        .duty_resolution = BUZZER_DUTY_RES,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return;
    }

    ledc_channel_config_t ch_cfg = {
        .gpio_num = BOARD_PIN_BUZZER,
        .speed_mode = BUZZER_LEDC_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .timer_sel = BUZZER_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&ch_cfg);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "ledc_channel_config failed: %s", esp_err_to_name(err));
        return;
    }

    buzzer_ledc_ready = true;
    FURI_LOG_I(TAG, "Init OK (GPIO buzzer, pin=%d)", BOARD_PIN_BUZZER);
}

void furi_hal_speaker_deinit(void) {
    furi_check(speaker_mutex != NULL);
    if(buzzer_ledc_ready) {
        ledc_stop(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    }
    buzzer_is_playing = false;
    buzzer_set_light_sleep_inhibited(false);
    FuriMutex* mutex = speaker_mutex;
    speaker_mutex = NULL; /* unpublish first: is_mine()/acquire() check it lock-free */
    furi_mutex_free(mutex);
}

bool furi_hal_speaker_acquire(uint32_t timeout) {
    furi_check(!FURI_IS_IRQ_MODE());
    FuriMutex* mutex = speaker_mutex;
    if(!mutex) return false; /* HAL released by an app, see furi_hal_speaker_deinit() */
    return furi_mutex_acquire(mutex, timeout) == FuriStatusOk;
}

void furi_hal_speaker_release(void) {
    furi_check(!FURI_IS_IRQ_MODE());
    furi_check(furi_hal_speaker_is_mine());
    furi_hal_speaker_stop();
    furi_check(furi_mutex_release(speaker_mutex) == FuriStatusOk);
}

bool furi_hal_speaker_is_mine(void) {
    if(FURI_IS_IRQ_MODE()) return true;
    /* An app (Doom, Wolf3D, ...) may have taken the speaker over via
     * furi_hal_speaker_deinit(): nobody owns it then. Notification still asks. */
    FuriMutex* mutex = speaker_mutex;
    return mutex && (furi_mutex_get_owner(mutex) == furi_thread_get_current_id());
}

void furi_hal_speaker_start(float frequency, float volume) {
    furi_check(furi_hal_speaker_is_mine());

    if(!buzzer_ledc_ready || volume <= 0.0f || frequency <= 0.0f) {
        furi_hal_speaker_stop();
        return;
    }

    const uint32_t tone_hz = (uint32_t)(frequency + 0.5f);
    esp_err_t err = tone_hz ? buzzer_set_frequency(tone_hz) : ESP_ERR_INVALID_ARG;
    if(err != ESP_OK) {
        FURI_LOG_W(TAG, "Buzzer cannot play %lu Hz: %s", (unsigned long)tone_hz, esp_err_to_name(err));
        furi_hal_speaker_stop();
        return;
    }

    uint32_t duty = buzzer_duty_for_volume(volume);

    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, duty);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
    buzzer_is_playing = duty > 0;
    buzzer_set_light_sleep_inhibited(buzzer_is_playing);
}

void furi_hal_speaker_start_gdo_mirror(const GpioPin* gdo_pin, float volume) {
    furi_check(furi_hal_speaker_is_mine());
    UNUSED(gdo_pin);
    UNUSED(volume);
}

void furi_hal_speaker_set_volume(float volume) {
    furi_check(furi_hal_speaker_is_mine());
    /* Only adjusts a running tone, as on the original: a stopped buzzer stays silent. */
    if(!buzzer_ledc_ready || !buzzer_is_playing) return;

    uint32_t duty = buzzer_duty_for_volume(volume);

    if(duty == 0) {
        furi_hal_speaker_stop();
        return;
    }

    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, duty);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
    buzzer_set_light_sleep_inhibited(true);
}

void furi_hal_speaker_stop(void) {
    furi_check(furi_hal_speaker_is_mine());
    if(!buzzer_ledc_ready) return;
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
    buzzer_is_playing = false;
    buzzer_set_light_sleep_inhibited(false);
}

bool furi_hal_speaker_is_playing(void) {
    return buzzer_is_playing;
}

#else

void furi_hal_speaker_init(void) {
}

void furi_hal_speaker_deinit(void) {
}

bool furi_hal_speaker_acquire(uint32_t timeout) {
    (void)timeout;
    return true;
}

void furi_hal_speaker_release(void) {
}

bool furi_hal_speaker_is_mine(void) {
    return true;
}

void furi_hal_speaker_start(float frequency, float volume) {
    (void)frequency;
    (void)volume;
}

void furi_hal_speaker_start_gdo_mirror(const GpioPin* gdo_pin, float volume) {
    (void)gdo_pin;
    (void)volume;
}

void furi_hal_speaker_set_volume(float volume) {
    (void)volume;
}

void furi_hal_speaker_stop(void) {
}

bool furi_hal_speaker_is_playing(void) {
    return false;
}

#endif
