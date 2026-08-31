#ifndef MP3_DECODER_H
#define MP3_DECODER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <furi.h>

/* Streaming MP3 decoder built on libhelix-mp3. Owns:
 *   - one Helix decoder instance
 *   - the storage file
 *   - the decoder thread that loops Read → Decode → Push-to-I2S
 *
 * Lifecycle:
 *   mp3_decoder_init();          // app start
 *   mp3_decoder_play(path);      // start track (any existing track is stopped first)
 *   mp3_decoder_pause();
 *   mp3_decoder_resume();
 *   mp3_decoder_stop();          // stops decoder thread, drains I2S
 *   mp3_decoder_deinit();        // app exit
 */

/* Callback fired (from the decoder thread) when the decoder reaches EOF
 * or hits an unrecoverable error. The app uses this to advance the
 * playlist / update UI. */
typedef void (*Mp3DecoderEndedCallback)(void* ctx);

bool mp3_decoder_init(void);
void mp3_decoder_deinit(void);

void mp3_decoder_set_ended_callback(Mp3DecoderEndedCallback cb, void* ctx);

/* Start decoding `path`. Stops any current playback first. Returns true if
 * the file was opened and decoder thread launched (decode itself is async). */
bool mp3_decoder_play(const char* path);

void mp3_decoder_pause(void);
void mp3_decoder_resume(void);

/* Seek to an absolute position (ms). No-op until the duration is known
 * (after the first decoded frame). CBR byte estimate + frame re-sync. */
void mp3_decoder_seek(uint32_t target_ms);
void mp3_decoder_stop(void);

bool mp3_decoder_is_playing(void);
bool mp3_decoder_is_paused(void);

/* Position in milliseconds, updated by the decoder thread.
 * `out_duration_ms` is 0 if the duration is unknown. */
void mp3_decoder_get_progress(uint32_t* out_elapsed_ms, uint32_t* out_duration_ms);

/* Probe the MP3 duration (ms) straight from the file, without playing it.
 * Returns 0 if unknown. Used for the DLNA/Cast progress bar, where the ESP
 * only serves the file and never decodes it. */
uint32_t mp3_decoder_probe_duration_ms(const char* path);

#endif /* MP3_DECODER_H */
