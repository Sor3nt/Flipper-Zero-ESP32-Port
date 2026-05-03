#ifndef MP3_PLAYER_H
#define MP3_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <furi.h>
#include <gui/gui.h>

#define MP3_PLAYER_TAG          "MP3Player"
#define MP3_PLAYER_DATA_DIR     "/ext/apps_data/mp3_player"
#define MP3_PLAYER_MAX_TRACKS   128
#define MP3_PLAYER_NAME_MAX     64

typedef struct {
    char name[MP3_PLAYER_NAME_MAX];
} Mp3Track;

typedef struct {
    Mp3Track* tracks;     /* PSRAM-allocated array, MP3_PLAYER_MAX_TRACKS slots */
    size_t    count;
} Mp3Playlist;

typedef enum {
    Mp3ViewBrowser,
    Mp3ViewNowPlaying,
} Mp3View;

typedef enum {
    Mp3StateIdle,
    Mp3StatePlaying,
    Mp3StatePaused,
} Mp3PlaybackState;

typedef struct {
    /* shared state */
    FuriMutex*        mutex;
    FuriMessageQueue* event_queue;

    /* playlist */
    Mp3Playlist playlist;
    int32_t     selected;            /* index in playlist, used by both views */

    /* current view */
    Mp3View view;

    /* playback */
    Mp3PlaybackState playback;
    int32_t          playing_index;  /* track currently being played (-1 if none) */
    uint32_t         elapsed_ms;     /* updated by decoder thread */
    uint32_t         duration_ms;    /* 0 if unknown */
    uint8_t          volume;         /* 0..100 */
} Mp3App;

typedef enum {
    Mp3EventTypeKey,
    Mp3EventTypeTick,
    Mp3EventTypeTrackEnded,
} Mp3EventType;

typedef struct {
    Mp3EventType type;
    InputEvent   input;
} Mp3Event;

#endif /* MP3_PLAYER_H */
