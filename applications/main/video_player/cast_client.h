#ifndef CAST_CLIENT_H
#define CAST_CLIENT_H

/* Google Cast (CASTV2) sender for the video player.
 *
 * Casts a media URL to a Cast-capable TV (Chromecast, Android/Google TV, Xiaomi
 * Mi TV, ...) by launching the Default Media Receiver (appId CC1AD845) and
 * issuing a LOAD. The ESP keeps serving the actual video over its HTTP range
 * server (dlna_render) — Cast only tells the TV which URL to fetch, exactly
 * like the DLNA path.
 *
 * Protocol: a TLS 1.2 connection to <ip>:8009 carrying length-prefixed
 * (uint32 big-endian) CastMessage protobufs, whose payload is JSON. Reuses the
 * hand-rolled protobuf + mbedTLS transport pattern from the Android TV remote,
 * minus the client certificate (Cast senders connect anonymously, VERIFY_NONE).
 *
 * A dedicated internal-DRAM session task owns the connection: it runs the
 * CONNECT -> LAUNCH -> LOAD handshake, answers heartbeat PINGs, tracks playback
 * position from MEDIA_STATUS, and executes transport commands set by the UI.
 * All mbedTLS/lwIP calls run on that task (never a FuriThread).
 */

#include <stdbool.h>
#include <stdint.h>

#define CAST_PORT 8009

typedef enum {
    CastStateIdle,
    CastStateConnecting, /* TLS + handshake + launch in progress */
    CastStatePlaying,
    CastStatePaused,
    CastStateFailed,
} CastState;

/* Start a Cast session to ip:port (network-order ip) and LOAD+autoplay the URL.
 * Non-blocking: spins up the session task. Poll cast_state(). mime is the
 * content type advertised to the receiver (e.g. "video/mp4"). */
bool cast_start(uint32_t ip, uint16_t port, const char* media_url, const char* mime, const char* title);

/* Load a different URL on the running session (re-LOAD). false if not active. */
bool cast_load(const char* media_url, const char* mime, const char* title);

/* Stop the session: STOP media + close TLS + join the task. Idempotent. */
void cast_stop(void);

CastState cast_state(void);

/* Transport controls — queue a command for the session task. */
void cast_ctrl_play(void);
void cast_ctrl_pause(void);
void cast_ctrl_seek(uint32_t position_sec);

/* Latest position/duration from MEDIA_STATUS (ms; 0 if unknown). */
void cast_get_progress(uint32_t* elapsed_ms, uint32_t* duration_ms);

#endif /* CAST_CLIENT_H */
