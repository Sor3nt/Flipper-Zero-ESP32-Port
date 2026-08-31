#include "video_storage.h"

#include <string.h>
#include <stdio.h>
#include <esp_heap_caps.h>

#define TAG VIDEO_PLAYER_TAG

static const char* const VIDEO_EXTS[] = {
    ".mp4", ".m4v", ".mkv", ".avi", ".mov", ".webm",
    ".ts", ".mpg", ".mpeg", ".wmv", ".flv", ".3gp",
};
#define VIDEO_EXT_COUNT (sizeof(VIDEO_EXTS) / sizeof(VIDEO_EXTS[0]))

bool video_storage_list_alloc(VideoList* list) {
    list->files =
        heap_caps_calloc(VIDEO_PLAYER_MAX_FILES, sizeof(VideoFile), MALLOC_CAP_SPIRAM);
    list->count = 0;
    return list->files != NULL;
}

void video_storage_list_free(VideoList* list) {
    if(list->files) {
        heap_caps_free(list->files);
        list->files = NULL;
    }
    list->count = 0;
}

static bool ends_with_ci(const char* name, const char* suffix) {
    size_t nl = strlen(name);
    size_t sl = strlen(suffix);
    if(nl < sl) return false;
    const char* tail = name + (nl - sl);
    for(size_t i = 0; i < sl; i++) {
        char a = tail[i];
        char b = suffix[i];
        if(a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if(b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if(a != b) return false;
    }
    return true;
}

static bool is_video(const char* name) {
    for(size_t i = 0; i < VIDEO_EXT_COUNT; i++) {
        if(ends_with_ci(name, VIDEO_EXTS[i])) return true;
    }
    return false;
}

size_t video_storage_scan(Storage* storage, VideoList* list) {
    if(!list->files) return 0;
    list->count = 0;

    storage_simply_mkdir(storage, VIDEO_PLAYER_DATA_DIR);

    File* dir = storage_file_alloc(storage);
    if(!storage_dir_open(dir, VIDEO_PLAYER_DATA_DIR)) {
        FURI_LOG_W(TAG, "scan: cannot open %s", VIDEO_PLAYER_DATA_DIR);
        storage_dir_close(dir);
        storage_file_free(dir);
        return 0;
    }

    char name[VIDEO_PLAYER_NAME_MAX];
    FileInfo info;
    while(list->count < VIDEO_PLAYER_MAX_FILES &&
          storage_dir_read(dir, &info, name, sizeof(name))) {
        if(info.flags & FSF_DIRECTORY) continue;
        if(!is_video(name)) continue;
        strncpy(list->files[list->count].name, name, VIDEO_PLAYER_NAME_MAX - 1);
        list->files[list->count].name[VIDEO_PLAYER_NAME_MAX - 1] = '\0';
        list->count++;
    }

    storage_dir_close(dir);
    storage_file_free(dir);

    FURI_LOG_I(TAG, "scan: %u video(s) in %s", (unsigned)list->count, VIDEO_PLAYER_DATA_DIR);
    return list->count;
}

bool video_storage_file_path(
    const VideoList* list, int32_t index, char* out_path, size_t out_size) {
    if(index < 0 || (size_t)index >= list->count) return false;
    snprintf(out_path, out_size, "%s/%s", VIDEO_PLAYER_DATA_DIR, list->files[index].name);
    return true;
}
