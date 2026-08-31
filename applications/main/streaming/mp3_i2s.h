#ifndef MP3_I2S_H
#define MP3_I2S_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* I2S_NUM_0 owner for the MP3 player. Pulls stereo int16 samples from a
 * ring buffer fed by the decoder thread. The sample rate can be changed
 * at runtime when a new file with a different rate starts.
 *
 * Typical lifecycle:
 *   furi_hal_speaker_deinit();
 *   mp3_i2s_init(44100);
 *   mp3_i2s_push(samples, n_frames);   // from decoder thread
 *   mp3_i2s_set_sample_rate(48000);    // when a new file starts
 *   mp3_i2s_deinit();
 */

/* Initialise the I2S peripheral and start the writer thread.
 * sample_rate is the initial rate; can be changed later with set_sample_rate. */
bool mp3_i2s_init(uint32_t sample_rate);

/* Stop writer thread and free I2S. */
void mp3_i2s_deinit(void);

/* Reconfigure the I2S sample rate (cheap, no DMA reallocation). */
void mp3_i2s_set_sample_rate(uint32_t sample_rate);

/* Software-gain in 0..100 (volume %). Applied in the writer loop. */
void mp3_i2s_set_volume(uint8_t volume);

/* Push n_frames stereo int16 samples (2*n_frames * sizeof(int16_t) bytes)
 * into the ring buffer. Blocks until space is available, up to timeout_ms.
 * Returns the number of frames actually written. */
size_t mp3_i2s_push(const int16_t* stereo_pcm, size_t n_frames, uint32_t timeout_ms);

/* Drop everything queued and silence the output. Used on stop/skip. */
void mp3_i2s_flush(void);

/* Return true while the ring buffer still holds queued audio. Used by the
 * decoder thread to detect "track really ended" vs. "decoder ahead of DAC". */
bool mp3_i2s_has_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* MP3_I2S_H */
