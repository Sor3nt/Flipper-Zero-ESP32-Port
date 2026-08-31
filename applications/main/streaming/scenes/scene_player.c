#include "../streaming.h"
#include "../stream_player.h"
#include "../airplay_raop.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static void player_cb(PlayerViewEvent ev, void* ctx) {
    StreamingApp* app = ctx;
    uint32_t custom;
    switch(ev) {
    case PlayerViewEventPlayPause:
        custom = StreamingEventPlayPause;
        break;
    case PlayerViewEventSeekBack:
        custom = StreamingEventSeekBack;
        break;
    case PlayerViewEventSeekForward:
        custom = StreamingEventSeekFwd;
        break;
    default:
        return;
    }
    view_dispatcher_send_custom_event(app->view_dispatcher, custom);
}

static void player_refresh_view(StreamingApp* app) {
    char title[STREAMING_NAME_MAX];
    strncpy(title, app->sel_name, sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    size_t len = strlen(title);
    if(len >= 4 &&
       (strcasecmp(title + len - 4, ".mp3") == 0 || strcasecmp(title + len - 4, ".mp4") == 0)) {
        title[len - 4] = '\0';
    }

    const char* target = "";
    if(app->play_mode != PlayModeLocal && app->have_device) {
        target = app->cur_device.name;
    }

    uint8_t state = (app->playback == PlaybackPlaying) ? 1 :
                    (app->playback == PlaybackPaused)  ? 2 : 0;

    player_view_update(
        app->player_view,
        title,
        target,
        app->elapsed_ms,
        app->duration_ms,
        state,
        stream_player_seekable(app),
        stream_player_is_connecting(app));
}

void streaming_scene_player_on_enter(void* context) {
    StreamingApp* app = context;

    if(!stream_player_start(app) && app->play_mode == PlayModeAirplay) {
        /* AirPlay handshake failed → show an error instead of a dead player.
         * AirPlay 2 devices are rejected before SETUP (no receiver crash). */
        snprintf(
            app->error_msg, sizeof(app->error_msg), "%s",
            airplay_raop_was_airplay2() ? "AirPlay 2 device\nis not supported" :
                                          "AirPlay connection\nfailed");
        scene_manager_next_scene(app->scene_manager, StreamingSceneError);
        return;
    }

    player_view_set_callback(app->player_view, player_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, StreamingViewPlayer);
    player_refresh_view(app);
}

bool streaming_scene_player_on_event(void* context, SceneManagerEvent event) {
    StreamingApp* app = context;

    if(event.type == SceneManagerEventTypeBack) {
        /* Back from the player returns to the file's action menu ("Play"/"Stream"),
         * skipping the device-scan / wifi scenes in between. The action menu is in
         * the stack for both the normal launch (browser below it) and the archive
         * arg-launch (action menu is the root); from there Back exits naturally. */
        if(!scene_manager_search_and_switch_to_previous_scene(
               app->scene_manager, StreamingSceneActionMenu)) {
            return false;
        }
        return true;
    }

    if(event.type == SceneManagerEventTypeTick) {
        stream_player_tick(app);
        player_refresh_view(app);
        return true;
    }

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case StreamingEventPlayPause:
            stream_player_toggle_pause(app);
            player_refresh_view(app);
            return true;
        case StreamingEventSeekBack:
            stream_player_seek(app, -5);
            return true;
        case StreamingEventSeekFwd:
            stream_player_seek(app, +5);
            return true;
        default:
            break;
        }
    }

    return false;
}

void streaming_scene_player_on_exit(void* context) {
    StreamingApp* app = context;
    stream_player_stop(app);
}
