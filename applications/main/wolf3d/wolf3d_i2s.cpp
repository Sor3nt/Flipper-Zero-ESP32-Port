/* wolf3d_i2s.cpp — I2S_NUM_0 owner für den Wolf3D-Port. Direkt aus
 * applications/main/doom/lib/doomgeneric/doom_i2s.c portiert (240 frames
 * pro chunk, dma_desc_num=6 → ~65 ms Puffer). */

#include "wolf3d_i2s.h"

#include <string.h>

#include <furi.h>
#include <boards/board.h>

#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>

#define TAG "WolfI2S"

#define CHUNK_FRAMES   240
#define DMA_DESC_NUM   6
#define DMA_FRAME_NUM  CHUNK_FRAMES

static i2s_chan_handle_t i2s_tx = nullptr;
static FuriThread*       writer_thread = nullptr;
static volatile bool     writer_run = false;
static int16_t*          tx_buf = nullptr;
static size_t            tx_buf_bytes = 0;

static inline int16_t clip16(int32_t v) {
    if(v > 32767) return 32767;
    if(v < -32768) return -32768;
    return (int16_t)v;
}

static int32_t writer_task(void*) {
    while(writer_run) {
        for(size_t i = 0; i < CHUNK_FRAMES; i++) {
            int16_t s = clip16(wolf3d_sfx_next_sample());
            tx_buf[i * 2 + 0] = s;
            tx_buf[i * 2 + 1] = s;
        }
        size_t written = 0;
        i2s_channel_write(i2s_tx, tx_buf, tx_buf_bytes, &written, 200);
    }
    return 0;
}

extern "C" bool wolf3d_i2s_init(void) {
    if(i2s_tx) return true;

    tx_buf_bytes = CHUNK_FRAMES * 2 * sizeof(int16_t);
    tx_buf = (int16_t*)heap_caps_malloc(tx_buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if(!tx_buf) {
        FURI_LOG_E(TAG, "tx_buf alloc failed");
        return false;
    }
    memset(tx_buf, 0, tx_buf_bytes);

    /* GPIO40 ist auf T-Embed sowohl LCD-RST als auch I2S-WS — vor Init reset. */
    gpio_reset_pin((gpio_num_t)BOARD_PIN_SPEAKER_WCLK);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    chan_cfg.auto_clear    = true;
    if(i2s_new_channel(&chan_cfg, &i2s_tx, nullptr) != ESP_OK) {
        FURI_LOG_E(TAG, "i2s_new_channel failed");
        goto err_chan;
    }

    {
        i2s_std_config_t std_cfg = {
            .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(WOLF3D_I2S_SAMPLE_RATE),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = (gpio_num_t)BOARD_PIN_SPEAKER_BCLK,
                .ws   = (gpio_num_t)BOARD_PIN_SPEAKER_WCLK,
                .dout = (gpio_num_t)BOARD_PIN_SPEAKER_DOUT,
                .din  = I2S_GPIO_UNUSED,
                .invert_flags = {0, 0, 0},
            },
        };
        if(i2s_channel_init_std_mode(i2s_tx, &std_cfg) != ESP_OK) {
            FURI_LOG_E(TAG, "init_std_mode failed");
            goto err_std;
        }
        if(i2s_channel_enable(i2s_tx) != ESP_OK) {
            FURI_LOG_E(TAG, "channel_enable failed");
            goto err_std;
        }
    }

    writer_run = true;
    writer_thread = furi_thread_alloc_ex("WolfI2S", 2048, writer_task, nullptr);
    furi_thread_set_priority(writer_thread, FuriThreadPriorityHigh);
    furi_thread_start(writer_thread);

    FURI_LOG_I(TAG, "Init OK @%d Hz", WOLF3D_I2S_SAMPLE_RATE);
    return true;

err_std:
    i2s_del_channel(i2s_tx);
    i2s_tx = nullptr;
err_chan:
    heap_caps_free(tx_buf);
    tx_buf = nullptr;
    return false;
}

extern "C" void wolf3d_i2s_deinit(void) {
    if(writer_thread) {
        writer_run = false;
        furi_thread_join(writer_thread);
        furi_thread_free(writer_thread);
        writer_thread = nullptr;
    }
    if(i2s_tx) {
        i2s_channel_disable(i2s_tx);
        i2s_del_channel(i2s_tx);
        i2s_tx = nullptr;
    }
    if(tx_buf) { heap_caps_free(tx_buf); tx_buf = nullptr; }
}
