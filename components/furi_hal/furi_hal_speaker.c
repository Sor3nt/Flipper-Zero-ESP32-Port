/**
 * @file furi_hal_speaker.c
 * Speaker HAL — I2S tone generation for boards with speaker hardware,
 * no-op stubs for boards without.
 *
 * Stays close to the STM32 Flipper Zero firmware API:
 *   - Mutex-based exclusive ownership (acquire/release/is_mine)
 *   - start(frequency, volume) / set_volume / stop
 *   - Cubic volume scaling (v^3) for perceptually linear loudness
 *
 * On ESP32 the speaker is driven via I2S (instead of STM32's PWM/TIM16).
 * A background thread continuously writes a sine-wave buffer to the I2S
 * peripheral; frequency and volume changes regenerate the buffer.
 */

#include "furi_hal_speaker.h"
#include "boards/board.h"
#include <furi.h>

#define TAG "FuriHalSpeaker"

#if BOARD_HAS_SPEAKER

#include <driver/i2s_std.h>
#include <driver/gpio.h>

#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Configuration ---- */
#define SPEAKER_SAMPLE_RATE   44100
#define SPEAKER_BITS          16
#define SPEAKER_DMA_DESC_NUM  4
#define SPEAKER_DMA_FRAME_NUM 256
#define SPEAKER_THREAD_STACK  2048

/* Maximum number of samples we pre-compute for one sine cycle.
 * For very low frequencies the cycle is long; we cap it to keep
 * memory usage reasonable (~4 KB for stereo 16-bit). */
#define SPEAKER_MAX_CYCLE_SAMPLES 1024

/* ---- Static state ---- */
static FuriMutex* speaker_mutex = NULL;
static i2s_chan_handle_t i2s_tx_handle = NULL;
static bool i2s_channel_enabled = false;

/* Tone state — protected by speaker_mutex for ownership,
 * individual fields accessed atomically by the writer thread. */
static volatile bool speaker_playing = false;
static volatile float speaker_frequency = 0.0f;
static volatile float speaker_volume = 0.0f;
static volatile bool speaker_buffer_dirty = true;

/* Background writer thread */
static FuriThread* speaker_thread = NULL;
static volatile bool speaker_thread_run = false;

/* Pre-computed waveform buffer (stereo interleaved: L, R, L, R, ...) */
static int16_t* wave_buffer = NULL;
static size_t wave_buffer_samples = 0; /* number of stereo frames */
static size_t wave_buffer_bytes = 0;

/* ---- Helpers ---- */

/** Apply cubic volume scaling (matches STM32 firmware behaviour). */
static inline float speaker_volume_curve(float v) {
    if(v < 0.0f) v = 0.0f;
    if(v > 1.0f) v = 1.0f;
    return v * v * v;
}

/** (Re-)generate the sine-wave buffer for the current frequency/volume. */
static void speaker_generate_buffer(void) {
    float freq = speaker_frequency;
    float vol = speaker_volume_curve(speaker_volume);

    if(freq < 1.0f) freq = 1.0f;

    /* Compute how many samples make up one full cycle */
    uint32_t samples_per_cycle = (uint32_t)(SPEAKER_SAMPLE_RATE / freq);
    if(samples_per_cycle < 2) samples_per_cycle = 2;
    if(samples_per_cycle > SPEAKER_MAX_CYCLE_SAMPLES) samples_per_cycle = SPEAKER_MAX_CYCLE_SAMPLES;

    /* Allocate / reallocate if needed */
    size_t needed = samples_per_cycle * 2 * sizeof(int16_t); /* stereo */
    if(wave_buffer == NULL || wave_buffer_samples != samples_per_cycle) {
        if(wave_buffer) free(wave_buffer);
        wave_buffer = malloc(needed);
        wave_buffer_samples = samples_per_cycle;
    }
    wave_buffer_bytes = needed;

    /* Fill with sine wave */
    float amplitude = vol * 32767.0f;
    for(uint32_t i = 0; i < samples_per_cycle; i++) {
        int16_t sample = (int16_t)(amplitude * sinf(2.0f * (float)M_PI * (float)i / (float)samples_per_cycle));
        wave_buffer[i * 2] = sample;     /* Left */
        wave_buffer[i * 2 + 1] = sample; /* Right */
    }

    speaker_buffer_dirty = false;
}

/** Background thread: continuously writes the waveform buffer to I2S. */
static int32_t speaker_writer_thread(void* context) {
    UNUSED(context);

    while(speaker_thread_run) {
        if(!speaker_playing) {
            furi_delay_ms(5);
            continue;
        }

        /* Regenerate buffer if frequency or volume changed */
        if(speaker_buffer_dirty) {
            speaker_generate_buffer();
        }

        if(wave_buffer && wave_buffer_bytes > 0) {
            size_t bytes_written = 0;
            i2s_channel_write(i2s_tx_handle, wave_buffer, wave_buffer_bytes, &bytes_written, 100);
        }
    }

    return 0;
}

/* ---- Public API ---- */

void furi_hal_speaker_init(void) {
    furi_assert(speaker_mutex == NULL);
    speaker_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    /* Release pin 40 from LCD RST usage — the display driver only pulses it
     * during init and does not need it afterwards. */
    gpio_reset_pin((gpio_num_t)BOARD_PIN_SPEAKER_WCLK);

    /* Configure I2S standard mode */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = SPEAKER_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = SPEAKER_DMA_FRAME_NUM;
    chan_cfg.auto_clear = true; /* send silence when no data */
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

    /* Start the writer thread (it idles until speaker_playing is set) */
    speaker_thread = furi_thread_alloc_ex("SpeakerWorker", SPEAKER_THREAD_STACK, speaker_writer_thread, NULL);
    speaker_thread_run = true;
    furi_thread_start(speaker_thread);

    FURI_LOG_I(TAG, "Init OK (I2S, BCLK=%d WS=%d DOUT=%d)",
               BOARD_PIN_SPEAKER_BCLK, BOARD_PIN_SPEAKER_WCLK, BOARD_PIN_SPEAKER_DOUT);
}

void furi_hal_speaker_deinit(void) {
    furi_check(speaker_mutex != NULL);

    /* Stop writer thread */
    speaker_thread_run = false;
    furi_thread_join(speaker_thread);
    furi_thread_free(speaker_thread);
    speaker_thread = NULL;

    /* Tear down I2S */
    if(i2s_channel_enabled) {
        i2s_channel_disable(i2s_tx_handle);
        i2s_channel_enabled = false;
    }
    i2s_del_channel(i2s_tx_handle);
    i2s_tx_handle = NULL;

    /* Free buffer */
    if(wave_buffer) {
        free(wave_buffer);
        wave_buffer = NULL;
    }

    furi_mutex_free(speaker_mutex);
    speaker_mutex = NULL;
}

bool furi_hal_speaker_acquire(uint32_t timeout) {
    furi_check(!FURI_IS_IRQ_MODE());

    if(furi_mutex_acquire(speaker_mutex, timeout) == FuriStatusOk) {
        /* Enable I2S channel on first acquire */
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
    return (FURI_IS_IRQ_MODE()) ||
           (furi_mutex_get_owner(speaker_mutex) == furi_thread_get_current_id());
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
    speaker_playing = true;
}

void furi_hal_speaker_set_volume(float volume) {
    furi_check(furi_hal_speaker_is_mine());

    if(volume <= 0.0f) {
        furi_hal_speaker_stop();
        return;
    }

    speaker_volume = volume;
    speaker_buffer_dirty = true;
}

void furi_hal_speaker_stop(void) {
    furi_check(furi_hal_speaker_is_mine());
    speaker_playing = false;
}

#else /* !BOARD_HAS_SPEAKER */

/* ---- No-op stubs for boards without speaker hardware ---- */

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

void furi_hal_speaker_set_volume(float volume) {
    (void)volume;
}

void furi_hal_speaker_stop(void) {
}

#endif /* BOARD_HAS_SPEAKER */
