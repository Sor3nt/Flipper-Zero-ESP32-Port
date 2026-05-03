#include "mp3_i2s.h"

#include <string.h>

#include <furi.h>
#include <boards/board.h>

#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>

#define TAG "Mp3I2S"

/* DMA buffer geometry. Helix produces 1152 stereo samples per frame,
 * so a chunk of 576 frames keeps the writer in lockstep with the decoder
 * (two writes per Helix frame) without making the latency too large. */
#define CHUNK_FRAMES   576
#define DMA_DESC_NUM   6
#define DMA_FRAME_NUM  CHUNK_FRAMES

/* Ring buffer holding decoded stereo samples (int16 L,R interleaved).
 * Sized for ~250 ms of audio at 44.1 kHz to absorb SD-read jitter. */
#define RB_FRAMES      11025  /* ~64 KB stereo @ int16 */

static i2s_chan_handle_t i2s_tx       = NULL;
static FuriThread*       writer_thread = NULL;
static volatile bool     writer_run   = false;
static FuriMutex*        rb_mutex     = NULL;
static FuriSemaphore*    rb_data_sem  = NULL;  /* signalled when push() adds data */

/* Stereo int16 ring buffer (PSRAM). Stores `int16_t` pairs so capacity is
 * RB_FRAMES * 2 elements. */
static int16_t* rb_buf  = NULL;
static size_t   rb_head = 0;  /* write position, in stereo frames */
static size_t   rb_tail = 0;  /* read position,  in stereo frames */
static size_t   rb_count = 0; /* frames currently held */

/* DMA-capable scratch the writer pulls into. */
static int16_t* tx_buf = NULL;

static volatile uint8_t  cur_volume = 80;  /* 0..100 */

static inline int16_t apply_gain(int16_t s, uint8_t vol) {
    /* Linear gain — fast enough for 4608 samples per Helix frame. */
    int32_t v = ((int32_t)s * vol) / 100;
    if(v > 32767) v = 32767;
    if(v < -32768) v = -32768;
    return (int16_t)v;
}

static int32_t mp3_i2s_writer_task(void* ctx) {
    (void)ctx;

    while(writer_run) {
        /* Pull up to CHUNK_FRAMES from the ring buffer. If empty, output
         * silence so the DMA never starves and the speaker stays glitch-free. */
        size_t got = 0;
        furi_mutex_acquire(rb_mutex, FuriWaitForever);
        size_t avail = rb_count;
        size_t take  = avail < CHUNK_FRAMES ? avail : CHUNK_FRAMES;
        for(size_t i = 0; i < take; i++) {
            tx_buf[i * 2 + 0] = rb_buf[rb_tail * 2 + 0];
            tx_buf[i * 2 + 1] = rb_buf[rb_tail * 2 + 1];
            rb_tail = (rb_tail + 1) % RB_FRAMES;
        }
        rb_count -= take;
        furi_mutex_release(rb_mutex);
        got = take;

        /* Pad the rest with silence. */
        if(got < CHUNK_FRAMES) {
            memset(&tx_buf[got * 2], 0, (CHUNK_FRAMES - got) * 2 * sizeof(int16_t));
        }

        /* Volume scaling. */
        uint8_t vol = cur_volume;
        if(vol != 100) {
            for(size_t i = 0; i < CHUNK_FRAMES * 2; i++) {
                tx_buf[i] = apply_gain(tx_buf[i], vol);
            }
        }

        size_t written = 0;
        i2s_channel_write(i2s_tx, tx_buf, CHUNK_FRAMES * 2 * sizeof(int16_t), &written, 200);

        /* If we got nothing this round, sleep briefly so we don't spin while
         * the decoder is still warming up (e.g. between tracks). */
        if(got == 0) {
            furi_semaphore_acquire(rb_data_sem, 20);
        }
    }

    return 0;
}

bool mp3_i2s_init(uint32_t sample_rate) {
    if(i2s_tx) return true;

    tx_buf = heap_caps_malloc(CHUNK_FRAMES * 2 * sizeof(int16_t),
                              MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if(!tx_buf) {
        FURI_LOG_E(TAG, "tx_buf alloc failed");
        return false;
    }
    memset(tx_buf, 0, CHUNK_FRAMES * 2 * sizeof(int16_t));

    rb_buf = heap_caps_malloc(RB_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if(!rb_buf) {
        FURI_LOG_E(TAG, "rb_buf alloc failed");
        goto err_rb;
    }
    rb_head = rb_tail = rb_count = 0;

    rb_mutex    = furi_mutex_alloc(FuriMutexTypeNormal);
    rb_data_sem = furi_semaphore_alloc(1, 0);

    /* GPIO40 is shared with LCD reset on the T-Embed. After boot it's still
     * configured as GPIO; release it before the I2S peripheral takes WS. */
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
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
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
    if(i2s_channel_init_std_mode(i2s_tx, &std_cfg) != ESP_OK) goto err_std;
    if(i2s_channel_enable(i2s_tx) != ESP_OK)                   goto err_std;

    writer_run = true;
    writer_thread = furi_thread_alloc_ex("Mp3I2S", 4096, mp3_i2s_writer_task, NULL);
    furi_thread_set_priority(writer_thread, FuriThreadPriorityHigh);
    furi_thread_start(writer_thread);

    FURI_LOG_I(TAG, "init OK @%lu Hz", (unsigned long)sample_rate);
    return true;

err_std:
    i2s_del_channel(i2s_tx);
    i2s_tx = NULL;
err_chan:
    furi_semaphore_free(rb_data_sem); rb_data_sem = NULL;
    furi_mutex_free(rb_mutex);        rb_mutex = NULL;
    heap_caps_free(rb_buf);           rb_buf = NULL;
err_rb:
    heap_caps_free(tx_buf);           tx_buf = NULL;
    return false;
}

void mp3_i2s_deinit(void) {
    if(writer_thread) {
        writer_run = false;
        /* Unblock the writer if it's parked on the data semaphore. */
        if(rb_data_sem) furi_semaphore_release(rb_data_sem);
        furi_thread_join(writer_thread);
        furi_thread_free(writer_thread);
        writer_thread = NULL;
    }
    if(i2s_tx) {
        i2s_channel_disable(i2s_tx);
        i2s_del_channel(i2s_tx);
        i2s_tx = NULL;
    }
    if(rb_data_sem) { furi_semaphore_free(rb_data_sem); rb_data_sem = NULL; }
    if(rb_mutex)    { furi_mutex_free(rb_mutex);        rb_mutex = NULL; }
    if(rb_buf)      { heap_caps_free(rb_buf);           rb_buf = NULL; }
    if(tx_buf)      { heap_caps_free(tx_buf);           tx_buf = NULL; }
}

void mp3_i2s_set_sample_rate(uint32_t sample_rate) {
    if(!i2s_tx) return;
    /* The new I2S API requires disable → reconfigure clk → enable. */
    i2s_channel_disable(i2s_tx);
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    i2s_channel_reconfig_std_clock(i2s_tx, &clk);
    i2s_channel_enable(i2s_tx);
    FURI_LOG_I(TAG, "sample rate -> %lu Hz", (unsigned long)sample_rate);
}

void mp3_i2s_set_volume(uint8_t volume) {
    if(volume > 100) volume = 100;
    cur_volume = volume;
}

size_t mp3_i2s_push(const int16_t* stereo_pcm, size_t n_frames, uint32_t timeout_ms) {
    if(!rb_buf || !rb_mutex) return 0;
    size_t pushed = 0;
    uint32_t start = furi_get_tick();

    while(pushed < n_frames) {
        furi_mutex_acquire(rb_mutex, FuriWaitForever);
        size_t free_slots = RB_FRAMES - rb_count;
        size_t take = n_frames - pushed;
        if(take > free_slots) take = free_slots;
        for(size_t i = 0; i < take; i++) {
            rb_buf[rb_head * 2 + 0] = stereo_pcm[(pushed + i) * 2 + 0];
            rb_buf[rb_head * 2 + 1] = stereo_pcm[(pushed + i) * 2 + 1];
            rb_head = (rb_head + 1) % RB_FRAMES;
        }
        rb_count += take;
        pushed   += take;
        furi_mutex_release(rb_mutex);

        if(take > 0) {
            /* Wake the writer thread if it's waiting. */
            furi_semaphore_release(rb_data_sem);
        }

        if(pushed >= n_frames) break;

        /* Buffer full — wait a bit for the writer to drain. */
        if(furi_get_tick() - start > furi_ms_to_ticks(timeout_ms)) break;
        furi_delay_ms(5);
    }
    return pushed;
}

void mp3_i2s_flush(void) {
    if(!rb_mutex) return;
    furi_mutex_acquire(rb_mutex, FuriWaitForever);
    rb_head = rb_tail = rb_count = 0;
    furi_mutex_release(rb_mutex);
}

bool mp3_i2s_has_pending(void) {
    if(!rb_mutex) return false;
    furi_mutex_acquire(rb_mutex, FuriWaitForever);
    bool any = rb_count > 0;
    furi_mutex_release(rb_mutex);
    return any;
}
