#include "streaming.h"
#include "stream_player.h"

#include <furi.h>
#include <storage/storage.h>
#include <wifi.h>
#include <wlan_app/views/wlan_lan_view.h>
#include <string.h>
#include <strings.h>

#define TAG STREAMING_TAG

static bool streaming_custom_event_callback(void* context, uint32_t event) {
    StreamingApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool streaming_back_event_callback(void* context) {
    StreamingApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void streaming_tick_event_callback(void* context) {
    StreamingApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

static StreamingApp* streaming_app_alloc(void) {
    StreamingApp* app = malloc(sizeof(StreamingApp));
    memset(app, 0, sizeof(StreamingApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->wifi = furi_record_open(RECORD_WIFI);

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&streaming_scene_handlers, app);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, streaming_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, streaming_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, streaming_tick_event_callback, 250);

    /* GUI modules */
    app->browser_path = furi_string_alloc();
    app->file_browser = file_browser_alloc(app->browser_path);
    app->submenu = submenu_alloc();
    app->text_input = text_input_alloc();
    app->popup = popup_alloc();
    app->loading = loading_alloc();
    app->player_view = player_view_alloc();
    app->view_lan = wlan_lan_view_alloc();
    wlan_lan_view_set_view_dispatcher(app->view_lan, app->view_dispatcher);

    view_dispatcher_add_view(
        app->view_dispatcher, StreamingViewFileBrowser, file_browser_get_view(app->file_browser));
    view_dispatcher_add_view(
        app->view_dispatcher, StreamingViewSubmenu, submenu_get_view(app->submenu));
    view_dispatcher_add_view(
        app->view_dispatcher, StreamingViewTextInput, text_input_get_view(app->text_input));
    view_dispatcher_add_view(
        app->view_dispatcher, StreamingViewPopup, popup_get_view(app->popup));
    view_dispatcher_add_view(
        app->view_dispatcher, StreamingViewLoading, loading_get_view(app->loading));
    view_dispatcher_add_view(
        app->view_dispatcher, StreamingViewPlayer, player_view_get_view(app->player_view));
    view_dispatcher_add_view(app->view_dispatcher, StreamingViewLan, app->view_lan);

    /* scan-result storage */
    app->ap_records = malloc(sizeof(StreamingApRecord) * STREAMING_MAX_APS);
    app->volume = 80;

    /* create the media directory so the browser has a root to open */
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, STREAMING_DATA_DIR);
    furi_record_close(RECORD_STORAGE);

    stream_player_init(app);

    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    return app;
}

static void streaming_app_free(StreamingApp* app) {
    stream_player_deinit(app);

    view_dispatcher_remove_view(app->view_dispatcher, StreamingViewFileBrowser);
    view_dispatcher_remove_view(app->view_dispatcher, StreamingViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, StreamingViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, StreamingViewPopup);
    view_dispatcher_remove_view(app->view_dispatcher, StreamingViewLoading);
    view_dispatcher_remove_view(app->view_dispatcher, StreamingViewPlayer);
    view_dispatcher_remove_view(app->view_dispatcher, StreamingViewLan);

    file_browser_free(app->file_browser);
    furi_string_free(app->browser_path);
    submenu_free(app->submenu);
    text_input_free(app->text_input);
    popup_free(app->popup);
    loading_free(app->loading);
    player_view_free(app->player_view);

    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);

    free(app->ap_records);

    furi_record_close(RECORD_WIFI);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t streaming_app(void* p) {
    UNUSED(p);
    FURI_LOG_I(TAG, "starting");

    StreamingApp* app = streaming_app_alloc();

    const char* arg = (const char*)p;
    if(arg && arg[0]) {
        /* Launched with a file path (e.g. from the archive browser clicking an
         * .mp3/.mp4). Skip the browser and open the action menu for that file. */
        strncpy(app->sel_path, arg, sizeof(app->sel_path) - 1);
        app->sel_path[sizeof(app->sel_path) - 1] = '\0';
        const char* slash = strrchr(arg, '/');
        const char* base = slash ? slash + 1 : arg;
        strncpy(app->sel_name, base, sizeof(app->sel_name) - 1);
        app->sel_name[sizeof(app->sel_name) - 1] = '\0';
        size_t len = strlen(base);
        app->sel_kind = MediaKindVideo;
        if(len >= 4 && strcasecmp(base + len - 4, ".mp3") == 0) {
            app->sel_kind = MediaKindAudio;
        }
        scene_manager_next_scene(app->scene_manager, StreamingSceneActionMenu);
    } else {
        scene_manager_next_scene(app->scene_manager, StreamingSceneBrowser);
    }

    view_dispatcher_run(app->view_dispatcher);
    streaming_app_free(app);

    FURI_LOG_I(TAG, "exit");
    return 0;
}
