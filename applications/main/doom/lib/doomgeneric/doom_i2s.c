/**
 * @file doom_i2s.c
 * I2S_NUM_0 owner for the Doom ESP32 port. See doom_i2s.h.
 *
 * The writer thread pulls samples from two optional sources per output
 * frame:
 *   - doom_sfx_next_sample() — one int32 sample from the SFX mixer
 *   - doom_music_next_block() — a block of int32 samples from the music mixer
 * Both return 0 (silence) if the sub-module is not initialised. The result
 * is clipped to int16 and duplicated into both L/R slots (the MAX98357A
 * sums them internally).
 */

#include "doom_i2s.h"

#include <string.h>

#include <furi.h>
#include <boards/board.h>

#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>

#define TAG "DoomI2S"

/* 240 samples per i2s_channel_write / mixer iteration. Matches dma_frame_num
 * so the driver never has to wait for an empty descriptor. */
#define CHUNK_FRAMES   240
#define DMA_DESC_NUM   6
#define DMA_FRAME_NUM  CHUNK_FRAMES

static i2s_chan_handle_t i2s_tx = NULL;
static FuriThread*       writer_thread = NULL;
static volatile bool     writer_run = false;

/* Scratch buffers — DMA-capable internal RAM for tx_buf, PSRAM is fine for
 * the intermediate mix accumulator. */
static int16_t* tx_buf = NULL;
static size_t   tx_buf_bytes = 0;
static int32_t* music_scratch = NULL; /* CHUNK_FRAMES int32 samples */

/* Weak sample-source hooks implemented by the SFX and music modules. If
 * a module is not compiled in, the weak default returns 0 (silence).
 * Using __attribute__((weak)) lets us link with or without the mixer
 * implementations without conditional compilation. */
__attribute__((weak)) int32_t doom_sfx_next_sample(void) { return 0; }
__attribute__((weak)) void    doom_music_fill_block(int32_t* out, size_t frames) {
    (void)out; (void)frames;
}

static inline int16_t clip16(int32_t v) {
    if(v > 32767) return 32767;
    if(v < -32768) return -32768;
    return (int16_t)v;
}

static int32_t doom_i2s_writer_task(void* ctx) {
    (void)ctx;

    while(writer_run) {
        /* Music generates a whole block (FM-synth is cheaper in batches). */
        if(music_scratch) {
            memset(music_scratch, 0, CHUNK_FRAMES * sizeof(int32_t));
            doom_music_fill_block(music_scratch, CHUNK_FRAMES);
        }

        for(size_t i = 0; i < CHUNK_FRAMES; i++) {
            int32_t mix = doom_sfx_next_sample();
            if(music_scratch) mix += music_scratch[i];
            int16_t s = clip16(mix);
            tx_buf[i * 2 + 0] = s;
            tx_buf[i * 2 + 1] = s;
        }

        size_t written = 0;
        i2s_channel_write(i2s_tx, tx_buf, tx_buf_bytes, &written, 200);
    }

    return 0;
}

bool doom_i2s_init(void) {
    if(i2s_tx) return true;

    tx_buf_bytes = CHUNK_FRAMES * 2 * sizeof(int16_t);
    tx_buf = heap_caps_malloc(tx_buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if(!tx_buf) {
        FURI_LOG_E(TAG, "tx_buf alloc failed (%u bytes)", (unsigned)tx_buf_bytes);
        return false;
    }
    memset(tx_buf, 0, tx_buf_bytes);

    music_scratch = heap_caps_malloc(CHUNK_FRAMES * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    if(music_scratch) {
        memset(music_scratch, 0, CHUNK_FRAMES * sizeof(int32_t));
    }

    /* Release GPIO40 from LCD-RST so the I2S peripheral can drive WS. */
    gpio_reset_pin((gpio_num_t)BOARD_PIN_SPEAKER_WCLK);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    chan_cfg.auto_clear    = true;
    if(i2s_new_channel(&chan_cfg, &i2s_tx, NULL) != ESP_OK) {
        FURI_LOG_E(TAG, "i2s_new_channel failed");
        goto err_chan;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(DOOM_I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)BOARD_PIN_SPEAKER_BCLK,
            .ws   = (gpio_num_t)BOARD_PIN_SPEAKER_WCLK,
            .dout = (gpio_num_t)BOARD_PIN_SPEAKER_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    if(i2s_channel_init_std_mode(i2s_tx, &std_cfg) != ESP_OK) {
        FURI_LOG_E(TAG, "i2s_channel_init_std_mode failed");
        goto err_std;
    }
    if(i2s_channel_enable(i2s_tx) != ESP_OK) {
        FURI_LOG_E(TAG, "i2s_channel_enable failed");
        goto err_std;
    }

    writer_run = true;
    writer_thread = furi_thread_alloc_ex("DoomI2S", 2048, doom_i2s_writer_task, NULL);
    furi_thread_set_priority(writer_thread, FuriThreadPriorityHigh);
    furi_thread_start(writer_thread);

    FURI_LOG_I(TAG, "Init OK @%d Hz", DOOM_I2S_SAMPLE_RATE);
    return true;

err_std:
    i2s_del_channel(i2s_tx);
    i2s_tx = NULL;
err_chan:
    heap_caps_free(tx_buf);
    tx_buf = NULL;
    if(music_scratch) { heap_caps_free(music_scratch); music_scratch = NULL; }
    return false;
}

void doom_i2s_deinit(void) {
    if(writer_thread) {
        writer_run = false;
        furi_thread_join(writer_thread);
        furi_thread_free(writer_thread);
        writer_thread = NULL;
    }
    if(i2s_tx) {
        i2s_channel_disable(i2s_tx);
        i2s_del_channel(i2s_tx);
        i2s_tx = NULL;
    }
    if(tx_buf) { heap_caps_free(tx_buf); tx_buf = NULL; }
    if(music_scratch) { heap_caps_free(music_scratch); music_scratch = NULL; }
}
