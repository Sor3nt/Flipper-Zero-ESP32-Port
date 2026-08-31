#include "video_player.h"
#include "video_storage.h"
#include "dlna_wifi.h"
#include "dlna_render.h"
#include "cast_client.h"

#include <stdio.h>
#include <string.h>

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <storage/storage.h>

#define TAG VIDEO_PLAYER_TAG

#define BROWSER_VISIBLE_ROWS 4
#define BROWSER_ROW_H        13
#define BROWSER_ROW_X_PAD    4
#define SEEK_STEP_SEC        30

/* Async playback commands (run on the WiFi worker via a helper FuriThread). */
typedef enum {
    VidCmdNone,
    VidCmdPlay,   /* start streaming pending_index to the TV */
    VidCmdPause,  /* pause on the TV */
    VidCmdResume, /* resume on the TV */
    VidCmdStop,   /* stop on the TV */
    VidCmdSeek,   /* seek to pending_seek seconds */
    VidCmdPoll,   /* GetPositionInfo → elapsed/duration */
    VidCmdVolume, /* RenderingControl SetVolume */
} VidCmd;

/* ---------- input / tick callbacks ---------- */

static void video_input_callback(InputEvent* input_event, void* ctx) {
    VideoApp* app = ctx;
    VideoEvent ev = {.type = VideoEventTypeKey, .input = *input_event};
    furi_message_queue_put(app->event_queue, &ev, FuriWaitForever);
}

static void video_tick_callback(void* ctx) {
    VideoApp* app = ctx;
    VideoEvent ev = {.type = VideoEventTypeTick};
    furi_message_queue_put(app->event_queue, &ev, 0);
}

/* ---------- helpers ---------- */

static void format_mmss(uint32_t ms, char* out, size_t out_size) {
    uint32_t total = ms / 1000;
    snprintf(out, out_size, "%02lu:%02lu", (unsigned long)(total / 60), (unsigned long)(total % 60));
}

static void file_display_name(const VideoFile* t, char* out, size_t out_size) {
    strncpy(out, t->name, out_size - 1);
    out[out_size - 1] = '\0';
    /* strip the extension for display */
    char* dot = strrchr(out, '.');
    if(dot && (out + strlen(out) - dot) <= 6) *dot = '\0';
}

static void build_target(VideoApp* app, DlnaTarget* t) {
    t->ip = app->cur_device.ip;
    t->port = app->cur_device.port;
    t->av_control = app->cur_device.av_control;
    t->rc_control = app->cur_device.rc_control;
}

/* ---------- async SOAP command (runs on the WiFi worker task) ---------- */

/* pending_cmd + args live in the app; this executes on the lwIP worker. */
static void cmd_do_soap(void* ctx) {
    VideoApp* app = ctx;
    DlnaTarget t;
    build_target(app, &t);
    bool ok = false;

    switch(app->pending_cmd) {
    case VidCmdPlay: {
        char url[416]; /* host + %-encoded name (name ×3) */
        char title[VIDEO_PLAYER_NAME_MAX];
        dlna_render_media_url(dlna_wifi_get_own_ip(), url, sizeof(url));
        if(app->pending_index >= 0 && (size_t)app->pending_index < app->list.count) {
            file_display_name(&app->list.files[app->pending_index], title, sizeof(title));
        } else {
            strncpy(title, "Video", sizeof(title) - 1);
        }
        ok = dlna_soap_set_and_play(&t, url, title);
        break;
    }
    case VidCmdPause:
        ok = dlna_soap_pause(&t);
        break;
    case VidCmdResume:
        ok = dlna_soap_play(&t);
        break;
    case VidCmdStop:
        ok = dlna_soap_stop(&t);
        break;
    case VidCmdSeek:
        ok = dlna_soap_seek(&t, app->pending_seek);
        break;
    case VidCmdPoll: {
        uint32_t el = 0, dur = 0;
        ok = dlna_soap_get_position(&t, &el, &dur);
        if(ok) {
            app->elapsed_ms = el;
            if(dur > 0) app->duration_ms = dur;
        }
        break;
    }
    case VidCmdVolume:
        ok = dlna_soap_set_volume(&t, app->volume);
        break;
    default:
        break;
    }
    app->cmd_result = ok;
}

/* FuriThread wrapper: hops onto the WiFi worker (which blocks it) so the UI
 * thread stays responsive. */
static int32_t cmd_thread_fn(void* ctx) {
    VideoApp* app = ctx;
    dlna_wifi_run_in_worker(cmd_do_soap, app);
    app->cmd_busy = false;
    return 0;
}

static void cmd_join(VideoApp* app) {
    if(app->cmd_thread) {
        furi_thread_join(app->cmd_thread);
        furi_thread_free(app->cmd_thread);
        app->cmd_thread = NULL;
    }
}

/* Start an async SOAP command. Returns false if one is already in flight. */
static bool cmd_start(VideoApp* app, VidCmd cmd) {
    if(app->cmd_busy) return false;
    if(!app->have_device) return false;
    cmd_join(app);
    app->pending_cmd = cmd;
    app->cmd_busy = true;
    app->cmd_thread = furi_thread_alloc_ex("VidDlnaCmd", 4096, cmd_thread_fn, app);
    furi_thread_start(app->cmd_thread);
    return true;
}

/* ---------- render: browser ---------- */

static void render_browser(Canvas* canvas, VideoApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Video Player");
    canvas_draw_line(canvas, 0, 12, 127, 12);

    canvas_set_font(canvas, FontSecondary);
    if(app->list.count == 0) {
        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, "No videos");
        canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignCenter, VIDEO_PLAYER_DATA_DIR);
        return;
    }

    int32_t top = app->selected - BROWSER_VISIBLE_ROWS / 2;
    if(top < 0) top = 0;
    int32_t max_top = (int32_t)app->list.count - BROWSER_VISIBLE_ROWS;
    if(max_top < 0) max_top = 0;
    if(top > max_top) top = max_top;

    int32_t end = top + BROWSER_VISIBLE_ROWS;
    if((size_t)end > app->list.count) end = (int32_t)app->list.count;

    char buf[VIDEO_PLAYER_NAME_MAX];
    for(int32_t i = top; i < end; i++) {
        int32_t row = i - top;
        int32_t y = 14 + row * BROWSER_ROW_H;
        bool selected = (i == app->selected);
        if(selected) {
            canvas_draw_box(canvas, 0, y, 128, BROWSER_ROW_H);
            canvas_invert_color(canvas);
        }
        file_display_name(&app->list.files[i], buf, sizeof(buf));
        buf[21] = '\0';
        canvas_draw_str(canvas, BROWSER_ROW_X_PAD, y + 10, buf);
        if(selected) canvas_invert_color(canvas);
    }
}

/* ---------- render: now-playing ---------- */

static void render_now_playing(Canvas* canvas, VideoApp* app) {
    canvas_set_font(canvas, FontPrimary);

    char title[VIDEO_PLAYER_NAME_MAX];
    if(app->playing_index >= 0 && (size_t)app->playing_index < app->list.count) {
        file_display_name(&app->list.files[app->playing_index], title, sizeof(title));
    } else {
        snprintf(title, sizeof(title), "—");
    }
    title[21] = '\0';
    canvas_draw_str(canvas, 2, 10, title);
    canvas_draw_line(canvas, 0, 12, 127, 12);

    canvas_set_font(canvas, FontSecondary);
    char elapsed[8], total[8];
    format_mmss(app->elapsed_ms, elapsed, sizeof(elapsed));
    if(app->duration_ms > 0) {
        format_mmss(app->duration_ms, total, sizeof(total));
    } else {
        strncpy(total, "--:--", sizeof(total));
    }
    canvas_draw_str(canvas, 2, 26, elapsed);
    canvas_draw_str_aligned(canvas, 126, 26, AlignRight, AlignBottom, total);

    int32_t bar_y = 30;
    int32_t bar_w = 124;
    canvas_draw_frame(canvas, 2, bar_y, bar_w, 6);
    if(app->duration_ms > 0) {
        /* 64-bit intermediate: elapsed_ms * bar_w overflows uint32 past ~9.8 h */
        uint32_t fill =
            (uint32_t)(((uint64_t)app->elapsed_ms * (uint32_t)(bar_w - 2)) / app->duration_ms);
        if(fill > (uint32_t)(bar_w - 2)) fill = bar_w - 2;
        canvas_draw_box(canvas, 3, bar_y + 1, fill, 4);
    }

    const char* status =
        (app->playback == VideoStatePlaying) ? "Playing" :
        (app->playback == VideoStatePaused)  ? "Paused"  : "Stopped";
    canvas_draw_str(canvas, 2, 50, status);

    /* target name on the right */
    char tgt[20];
    snprintf(tgt, sizeof(tgt), "%s", app->cur_device.name[0] ? app->cur_device.name : "TV");
    tgt[13] = '\0';
    canvas_draw_str_aligned(canvas, 126, 50, AlignRight, AlignBottom, tgt);

    elements_button_left(canvas, "Config");
    elements_button_center(canvas, app->playback == VideoStatePlaying ? "Pause" : "Play");
}

/* ---------- render: config (settings) ---------- */

#define VIDEO_CONFIG_ITEMS 5 /* DLNA Device, Repeat, Volume, Play Video, Exit */

static void render_config(Canvas* canvas, VideoApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Settings");
    canvas_draw_line(canvas, 0, 12, 127, 12);

    canvas_set_font(canvas, FontSecondary);

    char volbuf[8];
    snprintf(volbuf, sizeof(volbuf), "%u%%", (unsigned)app->volume);

    const char* labels[VIDEO_CONFIG_ITEMS] = {
        "Remote Device", "Repeat", "Volume", "Play Video", "Exit"};
    char devval[16];
    if(app->have_device) {
        snprintf(devval, sizeof(devval), "%s", app->cur_device.name);
        devval[10] = '\0';
    } else {
        strncpy(devval, "None", sizeof(devval));
    }
    const char* values[VIDEO_CONFIG_ITEMS] = {
        devval,
        app->repeat ? "On" : "Off",
        volbuf,
        NULL,
        NULL,
    };

    for(int idx = 0; idx < VIDEO_CONFIG_ITEMS; idx++) {
        int y = 13 + idx * 10;
        bool sel = (app->config_sel == idx);
        if(sel) {
            canvas_draw_box(canvas, 0, y, 128, 10);
            canvas_invert_color(canvas);
        }
        canvas_draw_str(canvas, 4, y + 8, labels[idx]);
        if(values[idx]) {
            char vb[16];
            if(idx == 2 && sel && app->config_editing) {
                snprintf(vb, sizeof(vb), "<%s>", values[idx]);
            } else {
                snprintf(vb, sizeof(vb), "%s", values[idx]);
            }
            canvas_draw_str_aligned(canvas, 124, y + 8, AlignRight, AlignBottom, vb);
        }
        if(sel) canvas_invert_color(canvas);
    }
}

static void video_render_callback(Canvas* canvas, void* ctx) {
    VideoApp* app = ctx;
    if(furi_mutex_acquire(app->mutex, 100) != FuriStatusOk) return;

    canvas_clear(canvas);
    if(dlna_ui_is_active(app->dlna)) {
        dlna_ui_render(canvas, app->dlna);
    } else if(app->view == VideoViewBrowser) {
        render_browser(canvas, app);
    } else if(app->view == VideoViewConfig) {
        render_config(canvas, app);
    } else {
        render_now_playing(canvas, app);
    }

    furi_mutex_release(app->mutex);
}

/* ---------- actions ---------- */

/* Ensure the HTTP file server is up (bound to our STA IP). */
static bool ensure_httpd(VideoApp* app) {
    if(app->httpd_up) return true;
    uint32_t ip = dlna_wifi_get_own_ip();
    if(!ip) return false;
    if(dlna_render_httpd_start(ip)) {
        app->httpd_up = true;
        return true;
    }
    return false;
}

static bool start_video(VideoApp* app, int32_t index) {
    if(index < 0 || (size_t)index >= app->list.count) return false;
    if(!app->have_device) return false;
    if(!ensure_httpd(app)) return false;

    char path[256];
    if(!video_storage_file_path(&app->list, index, path, sizeof(path))) return false;

    /* point the file server at the chosen file (both paths stream over HTTP) */
    dlna_render_set_file(path, app->list.files[index].name);

    app->playing_index = index;
    app->pending_index = index;
    app->elapsed_ms = 0;
    app->duration_ms = 0;
    app->view = VideoViewNowPlaying;

    if(app->cur_device.has_cast) {
        /* Google Cast path — the session task owns the connection. */
        app->use_cast = true;
        char url[416], title[VIDEO_PLAYER_NAME_MAX];
        dlna_render_media_url(dlna_wifi_get_own_ip(), url, sizeof(url));
        file_display_name(&app->list.files[index], title, sizeof(title));
        const char* mime = dlna_render_mime_for(app->list.files[index].name);
        CastState cs = cast_state();
        if(cs == CastStateIdle || cs == CastStateFailed) {
            cast_stop(); /* reap a previous/failed session before re-launching */
            if(cast_start(app->cur_device.ip, CAST_PORT, url, mime, title)) {
                app->playback = VideoStatePlaying;
                return true;
            }
            return false;
        }
        /* live session → just re-LOAD the new URL */
        cast_load(url, mime, title);
        app->playback = VideoStatePlaying;
        return true;
    }

    /* DLNA path — async SOAP command on the WiFi worker. */
    app->use_cast = false;
    if(app->cmd_busy) return false;
    if(cmd_start(app, VidCmdPlay)) {
        app->playback = VideoStatePlaying;
        return true;
    }
    return false;
}

static void toggle_pause(VideoApp* app) {
    if(app->use_cast) {
        if(app->playback == VideoStatePlaying) {
            cast_ctrl_pause();
            app->playback = VideoStatePaused;
        } else if(app->playback == VideoStatePaused) {
            cast_ctrl_play();
            app->playback = VideoStatePlaying;
        }
        return;
    }
    if(app->cmd_busy) return;
    if(app->playback == VideoStatePlaying) {
        if(cmd_start(app, VidCmdPause)) app->playback = VideoStatePaused;
    } else if(app->playback == VideoStatePaused) {
        if(cmd_start(app, VidCmdResume)) app->playback = VideoStatePlaying;
    }
}

static void do_seek(VideoApp* app, int delta_sec) {
    if(app->playback == VideoStateIdle) return;
    long pos = (long)(app->elapsed_ms / 1000) + delta_sec;
    if(pos < 0) pos = 0;
    if(app->duration_ms > 0 && pos > (long)(app->duration_ms / 1000))
        pos = app->duration_ms / 1000;
    if(app->use_cast) {
        cast_ctrl_seek((uint32_t)pos);
        return;
    }
    if(app->cmd_busy) return;
    app->pending_seek = (uint32_t)pos;
    cmd_start(app, VidCmdSeek);
}

static void volume_step(VideoApp* app, int delta) {
    int v = (int)app->volume + delta;
    if(v < 0) v = 0;
    if(v > 100) v = 100;
    app->volume = (uint8_t)v;
    /* push to the renderer if it supports RenderingControl */
    if(app->have_device && app->cur_device.rc_control[0] && !app->cmd_busy)
        cmd_start(app, VidCmdVolume);
}

static int32_t next_index(VideoApp* app) {
    int32_t count = (int32_t)app->list.count;
    if(count <= 0) return -1;
    int32_t n = app->playing_index + 1;
    return (n < count) ? n : -1;
}

/* ---------- input handling ---------- */

static bool handle_browser_input(VideoApp* app, const InputEvent* in) {
    if(in->type != InputTypeShort && in->type != InputTypeRepeat) return true;

    switch(in->key) {
    case InputKeyUp:
        if(app->list.count > 0)
            app->selected =
                (app->selected - 1 + (int32_t)app->list.count) % (int32_t)app->list.count;
        break;
    case InputKeyDown:
        if(app->list.count > 0)
            app->selected = (app->selected + 1) % (int32_t)app->list.count;
        break;
    case InputKeyOk:
        if(app->list.count > 0) {
            if(!app->have_device) {
                /* no TV chosen yet → open the DLNA setup first */
                dlna_ui_start_connect(app->dlna);
            } else {
                start_video(app, app->selected);
            }
        }
        break;
    case InputKeyBack:
        app->config_sel = 0;
        app->view = VideoViewConfig;
        break;
    default:
        break;
    }
    return true;
}

static bool handle_now_playing_input(VideoApp* app, const InputEvent* in) {
    if(in->type == InputTypeLong && in->key == InputKeyBack) {
        app->view = VideoViewBrowser;
        return true;
    }
    if(in->type != InputTypeShort && in->type != InputTypeRepeat) return true;

    switch(in->key) {
    case InputKeyUp:
        app->config_sel = 0;
        app->config_editing = false;
        app->view = VideoViewConfig;
        break;
    case InputKeyDown: {
        int32_t n = next_index(app);
        if(n >= 0) start_video(app, n);
        break;
    }
    case InputKeyLeft:
        do_seek(app, -SEEK_STEP_SEC);
        break;
    case InputKeyRight:
        do_seek(app, +SEEK_STEP_SEC);
        break;
    case InputKeyOk:
        if(app->playback == VideoStateIdle && app->playing_index >= 0) {
            start_video(app, app->playing_index);
        } else {
            toggle_pause(app);
        }
        break;
    case InputKeyBack:
        app->view = VideoViewBrowser;
        break;
    default:
        break;
    }
    return true;
}

static bool handle_config_input(VideoApp* app, const InputEvent* in) {
    if(in->type != InputTypeShort && in->type != InputTypeRepeat) return true;

    if(app->config_editing) {
        switch(in->key) {
        case InputKeyUp:
            volume_step(app, +5);
            break;
        case InputKeyDown:
            volume_step(app, -5);
            break;
        case InputKeyOk:
        case InputKeyBack:
        case InputKeyLeft:
            app->config_editing = false;
            break;
        default:
            break;
        }
        return true;
    }

    switch(in->key) {
    case InputKeyUp:
        app->config_sel = (app->config_sel - 1 + VIDEO_CONFIG_ITEMS) % VIDEO_CONFIG_ITEMS;
        break;
    case InputKeyDown:
        app->config_sel = (app->config_sel + 1) % VIDEO_CONFIG_ITEMS;
        break;
    case InputKeyOk:
        if(app->config_sel == 0) {
            dlna_ui_start_connect(app->dlna); /* DLNA device setup */
        } else if(app->config_sel == 1) {
            app->repeat = !app->repeat;
        } else if(app->config_sel == 2) {
            app->config_editing = true; /* volume edit */
        } else if(app->config_sel == 3) {
            app->view = VideoViewBrowser; /* Play Video → list */
        } else {
            return false; /* Exit */
        }
        break;
    case InputKeyBack:
        if(app->playback != VideoStateIdle && app->playing_index >= 0) {
            app->view = VideoViewNowPlaying;
        } else {
            return false; /* exit app */
        }
        break;
    default:
        break;
    }
    return true;
}

/* ---------- entry point ---------- */

int32_t video_player_app(void* p) {
    UNUSED(p);
    FURI_LOG_I(TAG, "starting");

    VideoApp* app = malloc(sizeof(VideoApp));
    memset(app, 0, sizeof(VideoApp));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->event_queue = furi_message_queue_alloc(16, sizeof(VideoEvent));
    app->view = VideoViewConfig;
    app->selected = 0;
    app->playback = VideoStateIdle;
    app->playing_index = -1;
    app->volume = 50;
    app->config_sel = 0;
    app->dlna = dlna_ui_alloc();

    if(!video_storage_list_alloc(&app->list)) {
        FURI_LOG_E(TAG, "list alloc failed");
        dlna_ui_free(app->dlna);
        furi_message_queue_free(app->event_queue);
        furi_mutex_free(app->mutex);
        free(app);
        return 255;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    video_storage_scan(storage, &app->list);
    furi_record_close(RECORD_STORAGE);

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, video_render_callback, app);
    view_port_input_callback_set(view_port, video_input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    FuriTimer* timer = furi_timer_alloc(video_tick_callback, FuriTimerTypePeriodic, app);
    furi_timer_start(timer, furi_kernel_get_tick_frequency() / 4); /* 4 Hz */

    uint8_t poll_div = 0;

    VideoEvent ev;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app->event_queue, &ev, 200) != FuriStatusOk) continue;

        furi_mutex_acquire(app->mutex, FuriWaitForever);

        switch(ev.type) {
        case VideoEventTypeKey:
            if(dlna_ui_is_active(app->dlna)) {
                dlna_ui_input(app->dlna, &ev.input);
            } else if(app->view == VideoViewBrowser) {
                running = handle_browser_input(app, &ev.input);
            } else if(app->view == VideoViewConfig) {
                running = handle_config_input(app, &ev.input);
            } else {
                running = handle_now_playing_input(app, &ev.input);
            }
            break;
        case VideoEventTypeTick:
            dlna_ui_tick(app->dlna);

            /* When the setup flow just picked a renderer, latch a stable copy. */
            if(!dlna_ui_is_active(app->dlna)) {
                const DlnaDevice* d = dlna_ui_target(app->dlna);
                if(d) {
                    if(!app->have_device || memcmp(&app->cur_device, d, sizeof(DlnaDevice)) != 0) {
                        app->cur_device = *d;
                        app->have_device = true;
                    }
                } else if(app->have_device) {
                    app->have_device = false;
                }
            }

            /* Reap a finished async command. */
            if(!app->cmd_busy && app->cmd_thread) {
                cmd_join(app);
            }

            /* Poll playback position ~every 1.5 s while playing. Cast keeps its
             * position live in the session task (MEDIA_STATUS); DLNA needs a
             * GetPositionInfo SOAP round-trip. */
            if(app->use_cast) {
                if(app->playback != VideoStateIdle) {
                    cast_get_progress(&app->elapsed_ms, &app->duration_ms);
                    CastState cs = cast_state();
                    if(cs == CastStatePlaying)
                        app->playback = VideoStatePlaying;
                    else if(cs == CastStatePaused)
                        app->playback = VideoStatePaused;
                    else if(cs == CastStateFailed || cs == CastStateIdle)
                        app->playback = VideoStateIdle;
                }
            } else if(
                app->playback == VideoStatePlaying && app->have_device && !app->cmd_busy &&
                !dlna_ui_is_active(app->dlna) && ++poll_div >= 6) {
                poll_div = 0;
                cmd_start(app, VidCmdPoll);
            }

            /* Auto-advance / repeat at end of playback. If the (re)start fails
             * (e.g. ensure_httpd can't get an IP), drop to Idle so we don't
             * retry every tick — start_video's early returns leave elapsed/
             * duration untouched, which would otherwise keep this true. */
            if(app->playback == VideoStatePlaying && app->duration_ms > 0 &&
               app->elapsed_ms + 1500 >= app->duration_ms && !app->cmd_busy) {
                int32_t n = (app->repeat && app->playing_index >= 0) ? app->playing_index
                                                                     : next_index(app);
                if(n >= 0 && start_video(app, n)) {
                    /* new playback started */
                } else {
                    app->playback = VideoStateIdle;
                    app->elapsed_ms = 0;
                }
            }
            break;
        }

        furi_mutex_release(app->mutex);
        view_port_update(view_port);
    }

    /* Teardown. */
    furi_timer_stop(timer);
    furi_timer_free(timer);

    /* Stop playback. cast_stop is idempotent (STOP media + close TLS + join the
     * session task). For DLNA, wait out any in-flight command, then send Stop. */
    cast_stop();
    cmd_join(app);
    if(!app->use_cast && app->have_device && app->playback != VideoStateIdle) {
        cmd_start(app, VidCmdStop);
        cmd_join(app);
    }

    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);

    if(app->httpd_up) dlna_render_httpd_stop();
    dlna_ui_free(app->dlna); /* stops WiFi + restores BT if started */
    video_storage_list_free(&app->list);
    furi_message_queue_free(app->event_queue);
    furi_mutex_free(app->mutex);
    free(app);
    FURI_LOG_I(TAG, "exit");
    return 0;
}
