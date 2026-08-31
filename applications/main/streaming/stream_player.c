#include "stream_player.h"
#include "mp3_decoder.h"
#include "mp3_sink.h"
#include "cast_client.h"
#include "dlna_render.h"

#include <furi.h>
#include <furi_hal.h>
#include <wlan_hal.h>
#include <string.h>
#include <stdio.h>
#include <storage/storage.h>

#define TAG          STREAMING_TAG

static inline uint32_t ms_to_ticks(uint32_t ms) {
    return (uint32_t)((uint64_t)ms * furi_kernel_get_tick_frequency() / 1000);
}
static inline uint32_t ticks_to_ms(uint32_t ticks) {
    return (uint32_t)((uint64_t)ticks * 1000 / furi_kernel_get_tick_frequency());
}

/* Parse an MP4 'moov/mvhd' box for the movie duration (ms). DLNA/Cast renderers
 * rarely report position for video, so we need a local estimate for the bar.
 * Returns 0 if not found. Runs in the UI thread (storage IO only, no lwIP). */
static uint32_t mp4_probe_duration_ms(const char* path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    uint32_t dur_ms = 0;

    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) goto out;
    uint64_t fsize = storage_file_size(file);

    /* Top-level boxes: [u32 size][4 char type][payload]. Find 'moov', then its
     * 'mvhd' child (timescale + duration). size==1 -> 64-bit largesize follows;
     * size==0 -> box runs to EOF. */
    uint64_t off = 0;
    while(off + 8 <= fsize) {
        uint8_t hdr[8];
        if(!storage_file_seek(file, off, true)) break;
        if(storage_file_read(file, hdr, 8) != 8) break;
        uint64_t sz = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                      ((uint32_t)hdr[2] << 8) | hdr[3];
        uint64_t body = off + 8;
        if(sz == 1) {
            uint8_t ext[8];
            if(storage_file_read(file, ext, 8) != 8) break;
            sz = 0;
            for(int i = 0; i < 8; i++) sz = (sz << 8) | ext[i];
            body = off + 16;
        } else if(sz == 0) {
            sz = fsize - off;
        }
        if(sz < (body - off)) break;

        if(memcmp(hdr + 4, "moov", 4) == 0) {
            uint64_t coff = body;
            uint64_t cend = off + sz;
            while(coff + 8 <= cend) {
                uint8_t ch[8];
                if(!storage_file_seek(file, coff, true)) break;
                if(storage_file_read(file, ch, 8) != 8) break;
                uint64_t csz = ((uint32_t)ch[0] << 24) | ((uint32_t)ch[1] << 16) |
                               ((uint32_t)ch[2] << 8) | ch[3];
                if(csz == 0) csz = cend - coff;
                if(csz < 8) break;
                if(memcmp(ch + 4, "mvhd", 4) == 0) {
                    uint8_t mv[32];
                    if(storage_file_read(file, mv, sizeof(mv)) == (int)sizeof(mv)) {
                        uint32_t timescale;
                        uint64_t duration;
                        if(mv[0] == 1) {
                            timescale = ((uint32_t)mv[20] << 24) | ((uint32_t)mv[21] << 16) |
                                        ((uint32_t)mv[22] << 8) | mv[23];
                            duration = 0;
                            for(int i = 24; i < 32; i++) duration = (duration << 8) | mv[i];
                        } else {
                            timescale = ((uint32_t)mv[12] << 24) | ((uint32_t)mv[13] << 16) |
                                        ((uint32_t)mv[14] << 8) | mv[15];
                            duration = ((uint32_t)mv[16] << 24) | ((uint32_t)mv[17] << 16) |
                                       ((uint32_t)mv[18] << 8) | mv[19];
                        }
                        if(timescale > 0)
                            dur_ms = (uint32_t)(((uint64_t)duration * 1000) / timescale);
                    }
                    break;
                }
                coff += csz;
            }
            break;
        }
        off += sz;
    }

out:
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return dur_ms;
}

/* Async DLNA SOAP commands, run on the WiFi worker via a helper FuriThread. */
typedef enum {
    DlnaCmdNone,
    DlnaCmdPlay,
    DlnaCmdPause,
    DlnaCmdResume,
    DlnaCmdStop,
    DlnaCmdSeek,
    DlnaCmdPoll,
    DlnaCmdVolume,
} DlnaCmd;

/* ---------- speaker HAL ownership (Local + AirPlay only) ---------- */

static bool stream_ensure_speaker(StreamingApp* app) {
    if(app->speaker_owned) return true;
    /* The standard speaker HAL owns I2S_NUM_0; take exclusive control. */
    furi_hal_speaker_deinit();
    if(!mp3_sink_init_speaker(44100)) {
        furi_hal_speaker_init();
        return false;
    }
    mp3_sink_set_volume(app->volume);
    app->speaker_owned = true;
    return true;
}

static void stream_release_speaker(StreamingApp* app) {
    if(!app->speaker_owned) return;
    mp3_decoder_stop();
    mp3_sink_deinit(); /* stops whichever sink (I2S or RAOP) is active */
    furi_hal_speaker_init();
    app->speaker_owned = false;
}

/* ---------- decoder EOF callback ---------- */

static void on_audio_ended(void* ctx) {
    StreamingApp* app = ctx;
    app->audio_ended = true;
}

/* ---------- DLNA target + async command ---------- */

static void build_target(StreamingApp* app, DlnaTarget* t) {
    t->ip = app->cur_device.ip;
    t->port = app->cur_device.port;
    t->av_control = app->cur_device.av_control;
    t->rc_control = app->cur_device.rc_control;
}

/* Runs on the WiFi worker (blocking lwIP socket calls are safe there). */
static void cmd_do_soap(void* ctx) {
    StreamingApp* app = ctx;
    DlnaTarget t;
    build_target(app, &t);
    bool ok = false;

    switch(app->pending_cmd) {
    case DlnaCmdPlay: {
        char url[416];
        char title[STREAMING_NAME_MAX];
        dlna_render_media_url(wlan_hal_get_own_ip(), url, sizeof(url));
        strncpy(title, app->sel_name, sizeof(title) - 1);
        title[sizeof(title) - 1] = '\0';
        ok = dlna_soap_set_and_play(&t, url, title);
        break;
    }
    case DlnaCmdPause:
        ok = dlna_soap_pause(&t);
        break;
    case DlnaCmdResume:
        ok = dlna_soap_play(&t);
        break;
    case DlnaCmdStop:
        ok = dlna_soap_stop(&t);
        break;
    case DlnaCmdSeek:
        ok = dlna_soap_seek(&t, app->pending_seek);
        break;
    case DlnaCmdPoll: {
        uint32_t el = 0, dur = 0;
        ok = dlna_soap_get_position(&t, &el, &dur);
        if(ok && el > 0) {
            /* This renderer has a working GetPositionInfo → use it and re-sync
             * the local clock so the two never drift apart. */
            app->dlna_pos_from_soap = true;
            app->elapsed_ms = el;
            app->dlna_play_tick = furi_get_tick() - ms_to_ticks(el);
            if(dur > 0) app->duration_ms = dur;
        }
        break;
    }
    case DlnaCmdVolume:
        ok = dlna_soap_set_volume(&t, app->volume);
        break;
    default:
        break;
    }
    app->cmd_result = ok;
}

static int32_t cmd_thread_fn(void* ctx) {
    StreamingApp* app = ctx;
    wlan_hal_run_in_worker(cmd_do_soap, app);
    app->cmd_busy = false;
    return 0;
}

static void cmd_join(StreamingApp* app) {
    if(app->cmd_thread) {
        furi_thread_join(app->cmd_thread);
        furi_thread_free(app->cmd_thread);
        app->cmd_thread = NULL;
    }
}

static bool cmd_start(StreamingApp* app, DlnaCmd cmd) {
    if(app->cmd_busy) return false;
    if(!app->have_device) return false;
    cmd_join(app);
    app->pending_cmd = cmd;
    app->cmd_busy = true;
    app->cmd_thread = furi_thread_alloc_ex("StrmDlnaCmd", 4096, cmd_thread_fn, app);
    furi_thread_start(app->cmd_thread);
    return true;
}

/* ---------- HTTP file server (Cast + DLNA) ---------- */

static bool ensure_httpd(StreamingApp* app) {
    if(app->httpd_up) return true;
    uint32_t ip = wlan_hal_get_own_ip();
    if(!ip) return false;
    if(dlna_render_httpd_start(ip)) {
        app->httpd_up = true;
        return true;
    }
    return false;
}

/* ---------- public API ---------- */

void stream_player_init(StreamingApp* app) {
    app->playback = PlaybackIdle;
    app->elapsed_ms = 0;
    app->duration_ms = 0;
    app->speaker_owned = false;
    app->audio_ended = false;
    app->httpd_up = false;
    app->cmd_busy = false;
    app->cmd_thread = NULL;
    mp3_decoder_init();
    mp3_decoder_set_ended_callback(on_audio_ended, app);
}

/* dlna_render_httpd_stop() calls lwip_shutdown(), which needs a real task
 * with lwIP thread-local storage. Calling it from the app FuriThread
 * (streaming_app_free) NULL-derefs in sys_thread_sem_get (LoadProhibited).
 * Route it through the WiFi worker, like the httpd/SOAP calls. */
static void httpd_stop_on_worker(void* ctx) {
    UNUSED(ctx);
    dlna_render_httpd_stop();
}

void stream_player_deinit(StreamingApp* app) {
    stream_player_stop(app);
    mp3_decoder_deinit();
    if(app->httpd_up) {
        wlan_hal_run_in_worker(httpd_stop_on_worker, NULL);
        app->httpd_up = false;
    }
}

bool stream_player_start(StreamingApp* app) {
    app->elapsed_ms = 0;
    app->duration_ms = 0;
    app->audio_ended = false;

    switch(app->play_mode) {
    case PlayModeLocal:
        if(!stream_ensure_speaker(app)) return false;
        mp3_sink_switch_speaker(); /* ensure the I2S sink (not RAOP) */
        if(!mp3_decoder_play(app->sel_path)) return false;
        app->playback = PlaybackPlaying;
        return true;

    case PlayModeAirplay:
        if(!stream_ensure_speaker(app)) return false;
        /* mp3_sink_switch_airplay tears down/recreates the I2S FuriThread AND
         * runs the (blocking) RTSP handshake. It MUST run in a FuriThread
         * context — we are on the view_dispatcher thread here, which is one.
         * Do NOT hop onto the wlan_hal worker: that is a raw xTaskCreate task
         * where furi_thread_alloc() dereferences a NULL TLS and crashes
         * (LoadProhibited). Blocks ~1-2 s (longer on an RTSP timeout). */
        if(!mp3_sink_switch_airplay(
               app->cur_device.ip, app->cur_device.port, wlan_hal_get_own_ip())) {
            return false;
        }
        mp3_sink_set_volume(app->volume);
        if(!mp3_decoder_play(app->sel_path)) return false;
        app->playback = PlaybackPlaying;
        return true;

    case PlayModeCast: {
        if(!ensure_httpd(app)) return false;
        dlna_render_set_file(app->sel_path, app->sel_name);
        char url[416];
        dlna_render_media_url(wlan_hal_get_own_ip(), url, sizeof(url));
        const char* mime = dlna_render_mime_for(app->sel_name);
        CastState cs = cast_state();
        if(cs == CastStateIdle || cs == CastStateFailed) {
            cast_stop();
            if(!cast_start(app->cur_device.ip, CAST_PORT, url, mime, app->sel_name))
                return false;
        } else {
            cast_load(url, mime, app->sel_name);
        }
        app->playback = PlaybackPlaying;
        return true;
    }

    case PlayModeDlna:
        if(!ensure_httpd(app)) return false;
        dlna_render_set_file(app->sel_path, app->sel_name);
        if(!cmd_start(app, DlnaCmdPlay)) return false;
        app->dlna_play_tick = furi_get_tick();
        app->dlna_pos_from_soap = false;
        /* The renderer usually can't report duration; estimate it from the MP3
         * file so the progress bar works (video stays unknown → no bar). */
        if(app->sel_kind == MediaKindAudio) {
            app->duration_ms = mp3_decoder_probe_duration_ms(app->sel_path);
        } else {
            app->duration_ms = mp4_probe_duration_ms(app->sel_path);
        }
        app->playback = PlaybackPlaying;
        return true;
    }
    return false;
}

void stream_player_toggle_pause(StreamingApp* app) {
    switch(app->play_mode) {
    case PlayModeLocal:
    case PlayModeAirplay:
        if(app->playback == PlaybackPlaying) {
            mp3_decoder_pause();
            app->playback = PlaybackPaused;
        } else if(app->playback == PlaybackPaused) {
            mp3_decoder_resume();
            app->playback = PlaybackPlaying;
        } else if(app->playback == PlaybackIdle) {
            stream_player_start(app); /* replay after end */
        }
        break;

    case PlayModeCast:
        if(cast_state() == CastStateConnecting) return; /* ignore until session is ready */
        if(app->playback == PlaybackPlaying) {
            cast_ctrl_pause();
            app->playback = PlaybackPaused;
        } else if(app->playback == PlaybackPaused) {
            cast_ctrl_play();
            app->playback = PlaybackPlaying;
        } else {
            stream_player_start(app);
        }
        break;

    case PlayModeDlna:
        if(app->cmd_busy) return;
        if(app->playback == PlaybackPlaying) {
            if(cmd_start(app, DlnaCmdPause)) app->playback = PlaybackPaused;
        } else if(app->playback == PlaybackPaused) {
            if(cmd_start(app, DlnaCmdResume)) {
                app->playback = PlaybackPlaying;
                /* resume the local clock from the frozen elapsed time */
                app->dlna_play_tick = furi_get_tick() - ms_to_ticks(app->elapsed_ms);
            }
        } else {
            stream_player_start(app);
        }
        break;
    }
}

void stream_player_seek(StreamingApp* app, int delta_sec) {
    if(app->playback == PlaybackIdle) return;
    if(!stream_player_seekable(app)) return;

    long pos = (long)(app->elapsed_ms / 1000) + delta_sec;
    if(pos < 0) pos = 0;
    if(app->duration_ms > 0 && pos > (long)(app->duration_ms / 1000))
        pos = app->duration_ms / 1000;

    if(app->play_mode == PlayModeLocal || app->play_mode == PlayModeAirplay) {
        mp3_decoder_seek((uint32_t)pos * 1000);
        app->elapsed_ms = (uint32_t)pos * 1000; /* instant UI feedback */
        return;
    }
    if(app->play_mode == PlayModeCast) {
        if(cast_state() == CastStateConnecting) return; /* session not ready */
        cast_ctrl_seek((uint32_t)pos);
        return;
    }
    /* DLNA (audio + video): REL_TIME seek. Audio (MP3) is robust; video (MP4)
     * is renderer-dependent — some renderers crash when the target is not on
     * a keyframe. The renderer maps REL_TIME to a byte range our server serves. */
    if(app->cmd_busy) return;
    app->pending_seek = (uint32_t)pos;
    app->elapsed_ms = (uint32_t)pos * 1000;
    app->dlna_play_tick = furi_get_tick() - ms_to_ticks(app->elapsed_ms);
    cmd_start(app, DlnaCmdSeek);
}

void stream_player_stop(StreamingApp* app) {
    /* Cast session (idempotent). */
    cast_stop();

    /* DLNA: wait out any in-flight command, then tell the TV to stop. */
    cmd_join(app);
    if(app->play_mode == PlayModeDlna && app->have_device && app->playback != PlaybackIdle) {
        cmd_start(app, DlnaCmdStop);
        cmd_join(app);
    }

    /* Local / AirPlay: stop the decoder and hand the speaker HAL back. */
    stream_release_speaker(app);

    app->playback = PlaybackIdle;
}

void stream_player_tick(StreamingApp* app) {
    switch(app->play_mode) {
    case PlayModeLocal:
    case PlayModeAirplay:
        if(mp3_decoder_is_playing() || mp3_decoder_is_paused()) {
            mp3_decoder_get_progress(&app->elapsed_ms, &app->duration_ms);
        }
        if(mp3_decoder_is_paused()) {
            app->playback = PlaybackPaused;
        } else if(mp3_decoder_is_playing()) {
            app->playback = PlaybackPlaying;
        }
        if(app->audio_ended) {
            app->audio_ended = false;
            app->playback = PlaybackIdle;
            app->elapsed_ms = 0;
        }
        /* push title/position to the AirPlay receiver ~1 Hz */
        if(app->play_mode == PlayModeAirplay && app->playback != PlaybackIdle) {
            static uint8_t div = 0;
            if(++div >= 4) {
                div = 0;
                mp3_sink_set_metadata(app->sel_name, app->duration_ms);
                mp3_sink_set_progress(app->elapsed_ms, app->duration_ms);
            }
        }
        break;

    case PlayModeCast:
        if(app->playback != PlaybackIdle) {
            cast_get_progress(&app->elapsed_ms, &app->duration_ms);
            CastState cs = cast_state();
            if(cs == CastStatePlaying)
                app->playback = PlaybackPlaying;
            else if(cs == CastStatePaused)
                app->playback = PlaybackPaused;
            else if(cs == CastStateFailed || cs == CastStateIdle)
                app->playback = PlaybackIdle;
        }
        break;

    case PlayModeDlna:
        if(!app->cmd_busy && app->cmd_thread) cmd_join(app);
        /* Local progress clock: most DLNA renderers have no working
         * GetPositionInfo, so count elapsed time from the play start. A working
         * poll re-syncs dlna_play_tick, keeping this accurate where the renderer
         * does report position. Frozen while paused. */
        if(app->playback == PlaybackPlaying) {
            app->elapsed_ms = ticks_to_ms(furi_get_tick() - app->dlna_play_tick);
        }
        if(app->playback == PlaybackPlaying && app->have_device && !app->cmd_busy) {
            static uint8_t div = 0;
            if(++div >= 6) {
                div = 0;
                cmd_start(app, DlnaCmdPoll);
            }
        }
        break;
    }
}

bool stream_player_seekable(StreamingApp* app) {
    UNUSED(app);
    /* All backends allow seeking. Local/AirPlay = MP3 decoder file-seek;
     * Cast = native; DLNA (audio + video) = REL_TIME. NB: DLNA *video* seek
     * is renderer-dependent — some crash when the target lands off a keyframe.
     * Kept enabled on purpose to test different receivers. */
    return true;
}

bool stream_player_is_connecting(StreamingApp* app) {
    return app->play_mode == PlayModeCast && cast_state() == CastStateConnecting;
}
