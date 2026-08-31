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

#define TAG          STREAMING_TAG

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
        if(ok) {
            app->elapsed_ms = el;
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

/* AirPlay RAOP handshake is blocking → run it on the WiFi worker. */
static void airplay_switch_worker(void* ctx) {
    StreamingApp* app = ctx;
    app->cmd_result = mp3_sink_switch_airplay(
        app->cur_device.ip, app->cur_device.port, wlan_hal_get_own_ip());
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

void stream_player_deinit(StreamingApp* app) {
    stream_player_stop(app);
    mp3_decoder_deinit();
    if(app->httpd_up) {
        dlna_render_httpd_stop();
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
        /* blocking RTSP handshake on the worker */
        app->cmd_result = false;
        wlan_hal_run_in_worker(airplay_switch_worker, app);
        if(!app->cmd_result) return false;
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
            if(cmd_start(app, DlnaCmdResume)) app->playback = PlaybackPlaying;
        } else {
            stream_player_start(app);
        }
        break;
    }
}

void stream_player_seek(StreamingApp* app, int delta_sec) {
    if(app->playback == PlaybackIdle) return;
    if(app->play_mode == PlayModeLocal || app->play_mode == PlayModeAirplay) return;

    long pos = (long)(app->elapsed_ms / 1000) + delta_sec;
    if(pos < 0) pos = 0;
    if(app->duration_ms > 0 && pos > (long)(app->duration_ms / 1000))
        pos = app->duration_ms / 1000;

    if(app->play_mode == PlayModeCast) {
        cast_ctrl_seek((uint32_t)pos);
        return;
    }
    if(app->cmd_busy) return;
    app->pending_seek = (uint32_t)pos;
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
    return app->play_mode == PlayModeCast || app->play_mode == PlayModeDlna;
}
