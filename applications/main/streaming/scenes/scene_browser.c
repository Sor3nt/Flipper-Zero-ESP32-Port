#include "../streaming.h"

#include <assets_icons.h>
#include <gui/icon_i.h>
#include <string.h>
#include <strings.h>

#define BrowserEventFileSelected 0xF001u

/* Uncompressed 10x10 icons (0x00 compression flag + raw XBM). Copied straight
 * into the file browser's custom-icon buffer (32 bytes), drawn with
 * canvas_draw_bitmap. Generated from the same PNG payloads as A_Streaming_14. */
static const uint8_t note_icon[21] = {
    0x00, 0x00, 0x00, 0xc0, 0x00, 0xc0, 0x00, 0xa0, 0x00, 0xa0, 0x00,
    0xa0, 0x00, 0x2c, 0x00, 0x3e, 0x00, 0x1e, 0x00, 0x0c, 0x00};
static const uint8_t video_icon[21] = {
    0x00, 0xff, 0x03, 0x01, 0x02, 0x09, 0x02, 0x19, 0x02, 0x39, 0x02,
    0x39, 0x02, 0x19, 0x02, 0x09, 0x02, 0x01, 0x02, 0xff, 0x03};

static bool browser_item_cb(FuriString* path, void* ctx, uint8_t** icon, FuriString* item_name) {
    UNUSED(ctx);
    UNUSED(item_name); /* left empty → browser fills the basename (ext stripped) */
    const char* p = furi_string_get_cstr(path);
    size_t len = strlen(p);
    const uint8_t* src = NULL;
    if(len >= 4) {
        const char* ext = p + len - 4;
        if(strcasecmp(ext, ".mp3") == 0)
            src = note_icon;
        else if(strcasecmp(ext, ".mp4") == 0)
            src = video_icon;
    }
    if(!src) return false;
    memcpy(*icon, src, sizeof(note_icon));
    return true;
}

static void browser_result_cb(void* ctx) {
    StreamingApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, BrowserEventFileSelected);
}

void streaming_scene_browser_on_enter(void* context) {
    StreamingApp* app = context;

    file_browser_configure(
        app->file_browser,
        ".mp3|.mp4",
        STREAMING_DATA_DIR,
        true, /* skip_assets */
        true, /* hide_dot_files */
        &I_music_10px, /* fallback icon (custom icons override per file) */
        true /* hide_ext */);
    file_browser_set_item_callback(app->file_browser, browser_item_cb, app);
    file_browser_set_callback(app->file_browser, browser_result_cb, app);

    FuriString* start = furi_string_alloc_set(STREAMING_DATA_DIR);
    file_browser_start(app->file_browser, start);
    furi_string_free(start);

    view_dispatcher_switch_to_view(app->view_dispatcher, StreamingViewFileBrowser);
}

bool streaming_scene_browser_on_event(void* context, SceneManagerEvent event) {
    StreamingApp* app = context;
    if(event.type != SceneManagerEventTypeCustom || event.event != BrowserEventFileSelected) {
        return false;
    }

    const char* path = furi_string_get_cstr(app->browser_path);
    strncpy(app->sel_path, path, sizeof(app->sel_path) - 1);
    app->sel_path[sizeof(app->sel_path) - 1] = '\0';

    /* basename */
    const char* slash = strrchr(path, '/');
    const char* base = slash ? slash + 1 : path;
    strncpy(app->sel_name, base, sizeof(app->sel_name) - 1);
    app->sel_name[sizeof(app->sel_name) - 1] = '\0';

    /* media kind by extension */
    size_t len = strlen(base);
    app->sel_kind = MediaKindVideo;
    if(len >= 4 && strcasecmp(base + len - 4, ".mp3") == 0) {
        app->sel_kind = MediaKindAudio;
    }

    scene_manager_next_scene(app->scene_manager, StreamingSceneActionMenu);
    return true;
}

void streaming_scene_browser_on_exit(void* context) {
    StreamingApp* app = context;
    file_browser_stop(app->file_browser);
}
