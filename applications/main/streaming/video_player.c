/* SPDX-License-Identifier: MIT */
#include "video_player.h"
#include "tvid_format.h"
#include <furi.h>
#include <furi_hal.h>
#include <boards/board.h>
#include <input/input.h>
#include <storage/storage.h>
#include <notification/notification_messages.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

bool video_player_is_local(const char* path) {
    const char* ext = strrchr(path, '.');
    return ext && strcasecmp(ext, ".tvid") == 0;
}

#if CONFIG_IDF_TARGET_ESP32S3 && BOARD_HAS_SPEAKER
#include <rom/tjpgd.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <driver/i2s_std.h>
#include <driver/gpio.h>

typedef struct {
    FuriMessageQueue* input;
    bool stopped;
    bool paused;
    bool have_frame;
    bool redraw;
    uint8_t volume;
    int64_t hud_until;
    uint8_t* jpeg;
    size_t jpeg_len, jpeg_pos;
    uint16_t* frame;
    uint16_t* stripe;
    int16_t* pcm;
    i2s_chan_handle_t audio;
    uint32_t frame_number;
    TvidHeader header;
} VideoSession;

static void input_cb(const void* value, void* ctx) {
    VideoSession* s = ctx;
    const InputEvent* e = value;
    if(e->type != InputTypeShort && e->type != InputTypeRepeat && e->type != InputTypeLong) return;
    /* Stop cannot be lost if rapid encoder events fill the queue. */
    if(e->key == InputKeyBack || (e->key == InputKeyOk && e->type == InputTypeLong)) {
        __atomic_store_n(&s->stopped, true, __ATOMIC_RELEASE);
    } else {
        furi_message_queue_put(s->input, e, 0);
    }
}
static bool stopped(VideoSession* s) {
    return __atomic_load_n(&s->stopped, __ATOMIC_ACQUIRE);
}
static bool read_exact(File* file, void* bytes, size_t size) {
    uint8_t* p = bytes;
    while(size) {
        size_t n = size > 16384 ? 16384 : size;
        if(storage_file_read(file, p, n) != n) return false;
        size -= n;
        p += n;
    }
    return true;
}
static UINT jpeg_read(JDEC* decoder, BYTE* dst, UINT count) {
    VideoSession* s = decoder->device;
    size_t available = s->jpeg_len - s->jpeg_pos;
    if(count > available) count = available;
    if(dst) memcpy(dst, s->jpeg + s->jpeg_pos, count);
    s->jpeg_pos += count;
    return count;
}
static UINT jpeg_write(JDEC* decoder, void* bitmap, JRECT* r) {
    VideoSession* s = decoder->device;
    if(stopped(s) || r->left > r->right || r->top > r->bottom ||
       r->right >= TVID_WIDTH || r->bottom >= TVID_HEIGHT) return 0;
    const uint8_t* rgb = bitmap; /* ROM decoder's fixed output is RGB888. */
    for(unsigned y = r->top; y <= r->bottom; ++y) {
        for(unsigned x = r->left; x <= r->right; ++x, rgb += 3) {
            uint16_t c = ((uint16_t)(rgb[0] & 0xf8) << 8) |
                         ((uint16_t)(rgb[1] & 0xfc) << 3) | (rgb[2] >> 3);
            s->frame[y * TVID_WIDTH + x] = (uint16_t)((c << 8) | (c >> 8));
        }
    }
    return 1;
}
static bool draw_frame(VideoSession* s) {
    if(!s->have_frame) return true;
    bool hud = s->paused || esp_timer_get_time() < s->hud_until;
    for(unsigned y = 0; y < TVID_HEIGHT; y += 8) {
        if(stopped(s)) return true;
        unsigned rows = TVID_HEIGHT - y;
        if(rows > 8) rows = 8;
        memcpy(s->stripe, s->frame + y * TVID_WIDTH, rows * TVID_WIDTH * 2);
        for(unsigned dy = 0; dy < rows; ++dy) {
            unsigned py = y + dy;
            for(unsigned x = 0; x < TVID_WIDTH; ++x) {
                uint16_t* pixel = s->stripe + dy * TVID_WIDTH + x;
                if(s->paused && py >= 8 && py < 26 && x >= 8 && x < 28)
                    *pixel = (x < 15 || x >= 21) ? 0xffff : 0;
                if(hud && py >= 157 && py < 163 && x >= 10 && x < 110)
                    *pixel = x - 10 < s->volume ? 0xffff : 0;
                if(py >= TVID_HEIGHT - 2)
                    *pixel = x < (uint64_t)s->frame_number * TVID_WIDTH / s->header.frames ? 0xe007 : 0;
            }
        }
        if(!furi_hal_display_draw_rgb565(0, y, TVID_WIDTH, rows, s->stripe)) return false;
    }
    s->redraw = false;
    return true;
}
static void handle_input(VideoSession* s) {
    InputEvent e;
    while(furi_message_queue_get(s->input, &e, 0) == FuriStatusOk) {
        if(e.key == InputKeyOk && e.type == InputTypeShort) s->paused = !s->paused;
        else if(e.key == InputKeyUp || e.key == InputKeyRight)
            s->volume = s->volume > 95 ? 100 : s->volume + 5;
        else if(e.key == InputKeyDown || e.key == InputKeyLeft)
            s->volume = s->volume < 5 ? 0 : s->volume - 5;
        else continue;
        s->hud_until = esp_timer_get_time() + 1500000;
        s->redraw = true;
    }
}
/* Pausing shifts the media clock; it does not consume the next frame. */
static bool wait_until(VideoSession* s, int64_t* deadline) {
    while(!stopped(s)) {
        int64_t before = esp_timer_get_time();
        handle_input(s);
        bool paused = s->paused;
        if(s->redraw && !draw_frame(s)) return false;
        if(!paused && before >= *deadline) break;
        furi_delay_ms(5);
        if(paused) *deadline += esp_timer_get_time() - before;
    }
    return true;
}
static bool audio_open(VideoSession* s) {
    gpio_reset_pin((gpio_num_t)BOARD_PIN_SPEAKER_WCLK);
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 4;
    chan.dma_frame_num = 256;
    chan.auto_clear = true;
    if(i2s_new_channel(&chan, &s->audio, NULL) != ESP_OK) return false;
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(TVID_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED,
                     .bclk = (gpio_num_t)BOARD_PIN_SPEAKER_BCLK,
                     .ws = (gpio_num_t)BOARD_PIN_SPEAKER_WCLK,
                     .dout = (gpio_num_t)BOARD_PIN_SPEAKER_DOUT,
                     .din = I2S_GPIO_UNUSED},
    };
    return i2s_channel_init_std_mode(s->audio, &cfg) == ESP_OK &&
           i2s_channel_enable(s->audio) == ESP_OK;
}
static bool audio_frame(VideoSession* s, size_t mono_bytes) {
    /* Expand backwards in the same allocation to interleaved stereo. */
    for(size_t i = mono_bytes / 2; i-- > 0;) {
        int16_t sample = (int32_t)s->pcm[i] * s->volume / 100;
        s->pcm[i * 2] = sample;
        s->pcm[i * 2 + 1] = sample;
    }
    size_t offset = 0, bytes = mono_bytes * 2;
    while(offset < bytes && !stopped(s)) {
        size_t take = bytes - offset;
        if(take > 1024) take = 1024;
        size_t written = 0;
        if(i2s_channel_write(s->audio, (uint8_t*)s->pcm + offset, take, &written, 100) != ESP_OK ||
           written != take) return false;
        offset += written;
    }
    return true;
}

bool video_player_run(Gui* gui, const char* path, uint8_t* volume, char* error, size_t error_size) {
    VideoSession* s = calloc(1, sizeof(*s));
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    FuriPubSub* inputs = NULL;
    FuriPubSubSubscription* subscription = NULL;
    NotificationApp* notification = NULL;
    void* work = NULL;
    bool result = false, direct = false, speaker_taken = false;
    const char* failure = "Video could not open";
    if(!s) goto cleanup;
    s->volume = *volume > 100 ? 100 : *volume;
    uint8_t header[TVID_HEADER_SIZE];
    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) goto cleanup;
    failure = "Invalid TVID file.\nUse the converter.";
    if(!read_exact(file, header, sizeof(header)) ||
       !tvid_parse_header(header, sizeof(header), &s->header)) goto cleanup;
    if(furi_hal_display_get_h_res() != TVID_WIDTH || furi_hal_display_get_v_res() != TVID_HEIGHT) {
        failure = "Requires T-Embed\n320 x 170 display"; goto cleanup;
    }
    uint64_t file_size = storage_file_size(file);
    if(file_size < TVID_HEADER_SIZE + (uint64_t)s->header.frames * 12) goto cleanup;
    failure = "Not enough memory\nfor video playback";
    s->jpeg = heap_caps_malloc(s->header.max_jpeg, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s->frame = heap_caps_malloc(TVID_WIDTH * TVID_HEIGHT * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s->stripe = heap_caps_malloc(TVID_WIDTH * 8 * 2, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    work = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(s->header.audio)
        s->pcm = heap_caps_malloc(TVID_RATE / s->header.fps * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!s->jpeg || !s->frame || !s->stripe || !work || (s->header.audio && !s->pcm)) goto cleanup;
    s->input = furi_message_queue_alloc(16, sizeof(InputEvent));
    if(!s->input) goto cleanup;
    if(s->header.audio) {
        furi_hal_speaker_deinit();
        speaker_taken = true;
        failure = "Speaker could not\nstart for video";
        if(!audio_open(s)) goto cleanup;
    }
    notification = furi_record_open(RECORD_NOTIFICATION);
    notification_message_block(notification, &sequence_display_backlight_enforce_on);
    furi_hal_power_insomnia_enter();
    gui_direct_draw_acquire(gui);
    direct = true;
    inputs = furi_record_open(RECORD_INPUT_EVENTS);
    subscription = furi_pubsub_subscribe(inputs, input_cb, s);
    s->hud_until = esp_timer_get_time() + 1500000;
    int64_t deadline = esp_timer_get_time();
    const int64_t frame_us = 1000000 / s->header.fps;
    for(uint32_t frame = 0; frame < s->header.frames && !stopped(s); ++frame) {
        /* Wait before decoding so pause keeps the last displayed frame intact. */
        if(!wait_until(s, &deadline)) { failure = "Video display error"; goto cleanup; }
        if(stopped(s)) break;
        uint8_t frame_header[8];
        uint32_t jpeg_size, pcm_size;
        failure = "Video file is\ntruncated or corrupt";
        if(!read_exact(file, frame_header, sizeof(frame_header))) goto cleanup;
        uint64_t position = storage_file_tell(file);
        if(position > file_size || !tvid_parse_frame(frame_header, &s->header, file_size - position,
                                                     &jpeg_size, &pcm_size)) goto cleanup;
        if(!read_exact(file, s->jpeg, jpeg_size) || !read_exact(file, s->pcm, pcm_size)) goto cleanup;
        s->jpeg_len = jpeg_size; s->jpeg_pos = 0;
        JDEC decoder = {0};
        failure = "Unsupported JPEG.\nReconvert the video.";
        if(jd_prepare(&decoder, jpeg_read, work, 4096, s) != JDR_OK ||
           decoder.width != TVID_WIDTH || decoder.height != TVID_HEIGHT) goto cleanup;
        JRESULT decoded = jd_decomp(&decoder, jpeg_write, 0);
        if(stopped(s)) break;
        if(decoded != JDR_OK) goto cleanup;
        /* No catch-up burst after a slow SD read: retain frame/audio pairing. */
        if(esp_timer_get_time() > deadline + frame_us) deadline = esp_timer_get_time();
        s->have_frame = true; s->frame_number = frame + 1;
        if(!draw_frame(s)) { failure = "Video display error"; goto cleanup; }
        if(pcm_size && !audio_frame(s, pcm_size)) { failure = "Video audio error"; goto cleanup; }
        deadline += frame_us;
    }
    if(!stopped(s) && !wait_until(s, &deadline)) { failure = "Video display error"; goto cleanup; }
    /* Let the last DMA block reach the speaker before deleting the channel. */
    if(!stopped(s) && s->audio) {
        deadline = esp_timer_get_time() + 64000;
        if(!wait_until(s, &deadline)) { failure = "Video display error"; goto cleanup; }
    }
    result = true;
cleanup:
    if(subscription) furi_pubsub_unsubscribe(inputs, subscription);
    if(inputs) furi_record_close(RECORD_INPUT_EVENTS);
    if(s && s->audio) {
        i2s_channel_disable(s->audio);
        i2s_del_channel(s->audio);
    }
    if(speaker_taken) furi_hal_speaker_init();
    if(direct) {
        gui_direct_draw_release(gui);
        furi_hal_power_insomnia_exit();
    }
    if(notification) {
        notification_message_block(notification, &sequence_display_backlight_enforce_auto);
        furi_record_close(RECORD_NOTIFICATION);
    }
    if(s) {
        *volume = s->volume;
        if(s->input) furi_message_queue_free(s->input);
        free(s->jpeg); free(s->frame); free(s->stripe); free(s->pcm);
        free(s);
    }
    free(work);
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    if(!result) snprintf(error, error_size, "%s", failure);
    return result;
}
#else
bool video_player_run(Gui* gui, const char* path, uint8_t* volume, char* error, size_t error_size) {
    UNUSED(gui); UNUSED(path); UNUSED(volume);
    snprintf(error, error_size, "Local video requires\nT-Embed ESP32-S3");
    return false;
}
#endif
