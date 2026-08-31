#ifndef AIRPLAY_RAOP_H
#define AIRPLAY_RAOP_H

/* RAOP (AirPlay 1) audio sender — UNENCRYPTED PCM only (receiver TXT et=0).
 *
 * Streams stereo 44.1 kHz int16 PCM to a RAOP receiver: RTSP handshake
 * (OPTIONS/ANNOUNCE/SETUP/RECORD/SET_PARAMETER) + an RTP/UDP audio stream with
 * the mandatory sync packets and timing replies. The blocking handshake must
 * run on a worker task (not the UI thread); streaming then runs on a dedicated
 * internal-DRAM sender task fed by a PSRAM ring buffer.
 *
 * Phase 1: PCM (SDP "L16/44100/2"), no ALAC, no crypto. Receivers that require
 * encryption (et!=0) are out of scope here.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* RTSP handshake + start the sender task. ips are network byte order, port is
 * the RAOP RTSP port from mDNS SRV. Blocking (call from a worker task). */
bool airplay_raop_start(uint32_t receiver_ip, uint16_t rtsp_port, uint32_t local_ip);

/* Tear down the RTSP session and stop the sender task. Idempotent. */
void airplay_raop_stop(void);

bool airplay_raop_is_active(void);

/* Push stereo int16 PCM frames (from the decoder). Blocks up to timeout_ms for
 * ring space; returns frames accepted. */
size_t airplay_raop_push(const int16_t* stereo_pcm, size_t n_frames, uint32_t timeout_ms);

/* Drop queued audio. */
void airplay_raop_flush(void);

/* True while the ring buffer still holds queued audio. */
bool airplay_raop_has_pending(void);

/* Volume 0..100 → AirPlay dB, sent via SET_PARAMETER. */
void airplay_raop_set_volume(uint8_t volume);

/* Track title (DAAP metadata) shown on the receiver. */
void airplay_raop_set_metadata(const char* title, uint32_t duration_ms);

/* Playback position/duration (ms) → progress SET_PARAMETER (receiver scrubber). */
void airplay_raop_set_progress(uint32_t elapsed_ms, uint32_t duration_ms);

#endif /* AIRPLAY_RAOP_H */
