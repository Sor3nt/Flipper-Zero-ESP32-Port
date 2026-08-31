#ifndef VIDEO_PLAYER_H
#define VIDEO_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <furi.h>
#include <gui/gui.h>

#include "dlna_ui.h"

#define VIDEO_PLAYER_TAG        "VideoPlayer"
#define VIDEO_PLAYER_DATA_DIR   "/ext/apps_data/video_player"
#define VIDEO_PLAYER_MAX_FILES  128
#define VIDEO_PLAYER_NAME_MAX   96

typedef struct {
    char name[VIDEO_PLAYER_NAME_MAX];
} VideoFile;

typedef struct {
    VideoFile* files;   /* PSRAM-allocated array, VIDEO_PLAYER_MAX_FILES slots */
    size_t     count;
} VideoList;

typedef enum {
    VideoViewBrowser,
    VideoViewNowPlaying,
    VideoViewConfig,
} VideoView;

typedef enum {
    VideoStateIdle,
    VideoStatePlaying,
    VideoStatePaused,
} VideoPlaybackState;

typedef struct {
    /* shared state */
    FuriMutex*        mutex;
    FuriMessageQueue* event_queue;

    /* list of videos on the SD */
    VideoList list;
    int32_t   selected;

    /* current view */
    VideoView view;

    /* playback (state mirrors what we told the TV via DLNA) */
    VideoPlaybackState playback;
    int32_t            playing_index; /* -1 if none */
    uint32_t           elapsed_ms;    /* polled from the TV (GetPositionInfo) */
    uint32_t           duration_ms;   /* 0 if unknown */
    uint8_t            volume;        /* 0..100, pushed via RenderingControl */
    bool               repeat;

    /* config (settings) screen */
    int32_t config_sel;
    bool    config_editing; /* volume item in edit mode */

    /* async playback command in flight (SOAP calls run on the WiFi worker) */
    volatile bool cmd_busy;
    FuriThread*   cmd_thread;
    int           pending_cmd; /* DlnaPlaybackCmd */
    int32_t       pending_index;
    uint32_t      pending_seek;

    /* DLNA output setup (WiFi connect + renderer discovery). Owns its own
     * sub-flow; the player delegates render/input to it while active. */
    DlnaUi* dlna;

    /* Stable copy of the chosen renderer (the SOAP async command reads it off
     * the UI thread, so it must not point back into the DlnaUi). */
    DlnaDevice    cur_device;
    bool          have_device;
    volatile bool cmd_result;
    bool          httpd_up;
    bool          use_cast; /* current playback goes via Google Cast, not DLNA */
} VideoApp;

typedef enum {
    VideoEventTypeKey,
    VideoEventTypeTick,
} VideoEventType;

typedef struct {
    VideoEventType type;
    InputEvent     input;
} VideoEvent;

#endif /* VIDEO_PLAYER_H */
