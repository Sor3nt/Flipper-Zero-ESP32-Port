#pragma once

#include "streaming.h"

/* Playback engine abstraction. Fans out to one of three backends selected by
 * app->play_mode:
 *   PlayModeLocal   — Helix decoder → I2S speaker (T-Embed only)
 *   PlayModeAirplay — Helix decoder → RAOP sender
 *   PlayModeCast    — HTTP range server + Google Cast (TV pulls the file)
 *   PlayModeDlna    — HTTP range server + DLNA AVTransport (TV pulls the file)
 *
 * The scene sets app->sel_path / app->play_mode / app->cur_device, then calls
 * stream_player_start(). The player view drives pause/seek; the scene tick
 * calls stream_player_tick() to refresh progress and reap async commands.
 */

void stream_player_init(StreamingApp* app);
void stream_player_deinit(StreamingApp* app);

/* Start playing app->sel_path in app->play_mode. Returns false on failure. */
bool stream_player_start(StreamingApp* app);

void stream_player_toggle_pause(StreamingApp* app);

/* Stop the current playback and free per-session resources (speaker HAL,
 * TV session). Safe to call repeatedly / on app exit. */
void stream_player_stop(StreamingApp* app);

/* Seek by delta seconds (Cast/DLNA only; no-op for Local/AirPlay). */
void stream_player_seek(StreamingApp* app, int delta_sec);

/* Refresh app->elapsed_ms/duration_ms/playback and reap finished commands. */
void stream_player_tick(StreamingApp* app);

/* True if the current backend supports seeking (Cast/DLNA). */
bool stream_player_seekable(StreamingApp* app);

/* True while a Cast session is still launching on the TV (no media yet). */
bool stream_player_is_connecting(StreamingApp* app);
