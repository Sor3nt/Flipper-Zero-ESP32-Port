#ifndef MP3_SINK_H
#define MP3_SINK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Audio output router. The decoder always calls mp3_sink_* and never needs to
 * know whether the active sink is the I2S speaker or the AirPlay RAOP sender.
 *
 * Switching to AirPlay tears down I2S and runs the (blocking) RTSP handshake, so
 * mp3_sink_switch_airplay() must be called from a worker/job task, and the
 * caller should pause the decoder around the switch to avoid pushing into a sink
 * that is being torn down. */

bool mp3_sink_init_speaker(uint32_t sample_rate); /* app start */
void mp3_sink_deinit(void); /* app exit — stops whichever sink is active */

/* Switch to AirPlay (deinit I2S + RTSP connect + start streaming). Blocking.
 * Stays on the speaker and returns false on failure. ips network byte order. */
bool mp3_sink_switch_airplay(uint32_t recv_ip, uint16_t rtsp_port, uint32_t local_ip);
bool mp3_sink_switch_speaker(void); /* stop AirPlay + re-init I2S */

bool mp3_sink_is_airplay(void);

void mp3_sink_set_sample_rate(uint32_t rate);
void mp3_sink_set_volume(uint8_t volume);

/* AirPlay-only metadata (ignored on the speaker sink). */
void mp3_sink_set_metadata(const char* title, uint32_t duration_ms);
void mp3_sink_set_progress(uint32_t elapsed_ms, uint32_t duration_ms);
size_t mp3_sink_push(const int16_t* stereo_pcm, size_t n_frames, uint32_t timeout_ms);
void mp3_sink_flush(void);
bool mp3_sink_has_pending(void);

#endif /* MP3_SINK_H */
