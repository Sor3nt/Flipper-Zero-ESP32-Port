#ifndef DLNA_RENDER_H
#define DLNA_RENDER_H

/* DLNA renderer control + the HTTP file server that feeds it.
 *
 * This is the video counterpart of airplay_raop: the piece that actually hands
 * the media to the receiver and drives playback. It has two halves:
 *
 *   1. A tiny raw-lwIP HTTP/1.1 file server on a dedicated internal-DRAM task.
 *      It serves exactly ONE file at a time (the video the user picked) from
 *      the SD card, with Range support (206 Partial Content) so the TV can seek
 *      and buffer. The server runs for the whole app session; the served file
 *      is swapped with dlna_render_set_file().
 *
 *   2. Blocking SOAP calls (AVTransport / RenderingControl) to the chosen TV:
 *      SetAVTransportURI + Play/Pause/Stop/Seek + GetPositionInfo, plus an
 *      optional RenderingControl SetVolume. These MUST run on the WiFi worker
 *      task (wlan_hal_run_in_worker) — they issue lwIP socket calls.
 *
 * All IPs are network byte order.
 */

#include <stdbool.h>
#include <stdint.h>

/* ---- HTTP file server (runs on its own task) ---- */

/* Start the file server bound to local_ip on DLNA_HTTP_PORT. Idempotent. */
bool dlna_render_httpd_start(uint32_t local_ip);

/* Stop the file server + join its task. Idempotent. */
void dlna_render_httpd_stop(void);

/* Set the SD path of the file to serve (e.g. /ext/apps_data/video_player/x.mp4)
 * and the basename used in the URL. Thread-safe (guarded by a mutex). */
void dlna_render_set_file(const char* sd_path, const char* url_name);

/* Content-type ("video/mp4", ...) for a filename, by extension. */
const char* dlna_render_mime_for(const char* name);

/* Build the media URL the TV should fetch: http://<local_ip>:<port>/<url_name>. */
void dlna_render_media_url(uint32_t local_ip, char* out, int out_sz);

/* The port the file server listens on. */
#define DLNA_HTTP_PORT 8973

/* ---- SOAP control (blocking, call on the WiFi worker) ---- */

typedef struct {
    uint32_t ip; /* device IP, network byte order */
    uint16_t port; /* device HTTP/SOAP port */
    const char* av_control; /* AVTransport controlURL path */
    const char* rc_control; /* RenderingControl controlURL path ("" = none) */
} DlnaTarget;

/* SetAVTransportURI(media_url, title) then Play. Returns true on HTTP 200. */
bool dlna_soap_set_and_play(const DlnaTarget* t, const char* media_url, const char* title);

bool dlna_soap_play(const DlnaTarget* t);
bool dlna_soap_pause(const DlnaTarget* t);
bool dlna_soap_stop(const DlnaTarget* t);

/* Seek to an absolute position (seconds) via REL_TIME. */
bool dlna_soap_seek(const DlnaTarget* t, uint32_t position_sec);

/* GetPositionInfo → elapsed/duration in ms (0 if unknown). Returns true on 200. */
bool dlna_soap_get_position(const DlnaTarget* t, uint32_t* elapsed_ms, uint32_t* duration_ms);

/* RenderingControl SetVolume (0..100). No-op + false if rc_control is empty. */
bool dlna_soap_set_volume(const DlnaTarget* t, uint8_t volume);

#endif /* DLNA_RENDER_H */
