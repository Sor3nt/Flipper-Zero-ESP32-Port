#include "../subghz_i.h"

#define PLAYLIST_FOLDER "/ext/subplaylist"
#define PLAYLIST_EXT ".txt"

static void subghz_scene_playlist_callback(SubGhzCustomEvent event, void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

void subghz_scene_playlist_on_enter(void* context) {
    SubGhz* subghz = context;

    subghz_playlist_set_callback(subghz->subghz_playlist, subghz_scene_playlist_callback, subghz);

    // Create playlist folder
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, PLAYLIST_FOLDER);
    furi_record_close(RECORD_STORAGE);

    // Open file browser
    FuriString* playlist_path = furi_string_alloc();
    furi_string_set(playlist_path, PLAYLIST_FOLDER);

    DialogsFileBrowserOptions browser_options;
    dialog_file_browser_set_basic_options(&browser_options, PLAYLIST_EXT, NULL);
    browser_options.hide_ext = false;

    bool file_selected =
        dialog_file_browser_show(subghz->dialogs, playlist_path, playlist_path, &browser_options);

    if(file_selected) {
        subghz_playlist_set_file_path(
            subghz->subghz_playlist, furi_string_get_cstr(playlist_path));
        subghz_playlist_start(subghz->subghz_playlist);
        view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdPlaylist);
    } else {
        scene_manager_search_and_switch_to_previous_scene(
            subghz->scene_manager, SubGhzSceneStart);
    }

    furi_string_free(playlist_path);
}

bool subghz_scene_playlist_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventViewReceiverBack) {
            scene_manager_search_and_switch_to_previous_scene(
                subghz->scene_manager, SubGhzSceneStart);
            return true;
        }
    }
    return false;
}

void subghz_scene_playlist_on_exit(void* context) {
    SubGhz* subghz = context;
    subghz_playlist_stop(subghz->subghz_playlist);
}
