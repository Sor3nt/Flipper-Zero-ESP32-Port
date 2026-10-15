#include "../streaming.h"
#include "../video_player.h"
#include <stdio.h>

#include <wifi.h>
#include <string.h>
#include <strings.h>

#define ActionPlay   0u
#define ActionStream 1u
#define ActionVideoHelp 2u

static void action_submenu_cb(void* ctx, uint32_t index) {
    StreamingApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void streaming_scene_action_menu_on_enter(void* context) {
    StreamingApp* app = context;

    /* Header: file name without extension. */
    char header[STREAMING_NAME_MAX];
    strncpy(header, app->sel_name, sizeof(header) - 1);
    header[sizeof(header) - 1] = '\0';
    size_t hlen = strlen(header);
    if(hlen >= 4 &&
       (strcasecmp(header + hlen - 4, ".mp3") == 0 || strcasecmp(header + hlen - 4, ".mp4") == 0)) {
        header[hlen - 4] = '\0';
    }

    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, header);
    if(app->sel_kind == MediaKindAudio) {
        submenu_add_item(app->submenu, "Play", ActionPlay, action_submenu_cb, app);
    }
    if(video_player_is_local(app->sel_path)) {
        submenu_add_item(app->submenu, "Play on screen", ActionPlay, action_submenu_cb, app);
    } else {
        submenu_add_item(app->submenu, "Stream", ActionStream, action_submenu_cb, app);
        if(app->sel_kind == MediaKindVideo)
            submenu_add_item(app->submenu, "Local video help", ActionVideoHelp, action_submenu_cb, app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, StreamingViewSubmenu);
}

bool streaming_scene_action_menu_on_event(void* context, SceneManagerEvent event) {
    StreamingApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == ActionPlay) {
        if(video_player_is_local(app->sel_path)) {
            if(!video_player_run(app->gui, app->sel_path, &app->volume,
                                 app->error_msg, sizeof(app->error_msg)))
                scene_manager_next_scene(app->scene_manager, StreamingSceneError);
            return true;
        }
        /* Existing local MP3 playback. */
        app->play_mode = PlayModeLocal;
        scene_manager_next_scene(app->scene_manager, StreamingScenePlayer);
        return true;
    }

    if(event.event == ActionVideoHelp) {
        snprintf(app->error_msg, sizeof(app->error_msg),
                 "Convert MP4 to TVID\nusing convert_video.py\nthen Play on screen");
        scene_manager_next_scene(app->scene_manager, StreamingSceneError);
        return true;
    }

    if(event.event == ActionStream) {
        Wifi* wifi = app->wifi;
        if(wifi_is_connected(wifi)) {
            scene_manager_next_scene(app->scene_manager, StreamingSceneDeviceScan);
        } else {
            scene_manager_next_scene(app->scene_manager, StreamingSceneWifiScan);
        }
        return true;
    }

    return false;
}

void streaming_scene_action_menu_on_exit(void* context) {
    StreamingApp* app = context;
    submenu_reset(app->submenu);
}
