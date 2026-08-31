#include "mp3_sink.h"
#include "mp3_i2s.h"
#include "airplay_raop.h"

static bool s_airplay = false;
static uint32_t s_rate = 44100;
static uint8_t s_vol = 80;

bool mp3_sink_init_speaker(uint32_t sample_rate) {
    s_rate = sample_rate;
    s_airplay = false;
    return mp3_i2s_init(sample_rate);
}

void mp3_sink_deinit(void) {
    if(s_airplay) {
        airplay_raop_stop();
        s_airplay = false;
    } else {
        mp3_i2s_deinit();
    }
}

bool mp3_sink_switch_airplay(uint32_t recv_ip, uint16_t rtsp_port, uint32_t local_ip) {
    if(s_airplay) return true;
    mp3_i2s_deinit(); /* free the I2S DMA + writer before we stream over WiFi */
    if(!airplay_raop_start(recv_ip, rtsp_port, local_ip)) {
        mp3_i2s_init(s_rate); /* restore the speaker on failure */
        mp3_i2s_set_volume(s_vol);
        return false;
    }
    airplay_raop_set_volume(s_vol);
    s_airplay = true;
    return true;
}

bool mp3_sink_switch_speaker(void) {
    if(!s_airplay) return true;
    airplay_raop_stop();
    s_airplay = false;
    bool ok = mp3_i2s_init(s_rate);
    mp3_i2s_set_volume(s_vol);
    return ok;
}

bool mp3_sink_is_airplay(void) {
    return s_airplay;
}

void mp3_sink_set_sample_rate(uint32_t rate) {
    s_rate = rate;
    if(!s_airplay) mp3_i2s_set_sample_rate(rate);
    /* RAOP session is fixed at 44.1 kHz (resampling is a later phase). */
}

void mp3_sink_set_volume(uint8_t volume) {
    s_vol = volume;
    if(s_airplay) {
        airplay_raop_set_volume(volume);
    } else {
        mp3_i2s_set_volume(volume);
    }
}

void mp3_sink_set_metadata(const char* title, uint32_t duration_ms) {
    if(s_airplay) airplay_raop_set_metadata(title, duration_ms);
}

void mp3_sink_set_progress(uint32_t elapsed_ms, uint32_t duration_ms) {
    if(s_airplay) airplay_raop_set_progress(elapsed_ms, duration_ms);
}

size_t mp3_sink_push(const int16_t* stereo_pcm, size_t n_frames, uint32_t timeout_ms) {
    if(s_airplay) return airplay_raop_push(stereo_pcm, n_frames, timeout_ms);
    return mp3_i2s_push(stereo_pcm, n_frames, timeout_ms);
}

void mp3_sink_flush(void) {
    if(s_airplay) {
        airplay_raop_flush();
    } else {
        mp3_i2s_flush();
    }
}

bool mp3_sink_has_pending(void) {
    return s_airplay ? airplay_raop_has_pending() : mp3_i2s_has_pending();
}
