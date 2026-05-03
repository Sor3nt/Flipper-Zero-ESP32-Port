#include "mp3_storage.h"

#include <string.h>
#include <stdio.h>
#include <esp_heap_caps.h>

#define TAG MP3_PLAYER_TAG

bool mp3_storage_playlist_alloc(Mp3Playlist* playlist) {
    playlist->tracks =
        heap_caps_calloc(MP3_PLAYER_MAX_TRACKS, sizeof(Mp3Track), MALLOC_CAP_SPIRAM);
    playlist->count = 0;
    return playlist->tracks != NULL;
}

void mp3_storage_playlist_free(Mp3Playlist* playlist) {
    if(playlist->tracks) {
        heap_caps_free(playlist->tracks);
        playlist->tracks = NULL;
    }
    playlist->count = 0;
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

size_t mp3_storage_scan(Storage* storage, Mp3Playlist* playlist) {
    if(!playlist->tracks) return 0;
    playlist->count = 0;

    /* Make sure the data directory exists. storage_simply_mkdir is idempotent. */
    storage_simply_mkdir(storage, MP3_PLAYER_DATA_DIR);

    File* dir = storage_file_alloc(storage);
    if(!storage_dir_open(dir, MP3_PLAYER_DATA_DIR)) {
        FURI_LOG_W(TAG, "scan: cannot open %s", MP3_PLAYER_DATA_DIR);
        storage_dir_close(dir);
        storage_file_free(dir);
        return 0;
    }

    char name[MP3_PLAYER_NAME_MAX];
    FileInfo info;
    while(playlist->count < MP3_PLAYER_MAX_TRACKS &&
          storage_dir_read(dir, &info, name, sizeof(name))) {
        if(info.flags & FSF_DIRECTORY) continue;
        if(!ends_with_ci(name, ".mp3")) continue;
        strncpy(playlist->tracks[playlist->count].name, name, MP3_PLAYER_NAME_MAX - 1);
        playlist->tracks[playlist->count].name[MP3_PLAYER_NAME_MAX - 1] = '\0';
        playlist->count++;
    }

    storage_dir_close(dir);
    storage_file_free(dir);

    FURI_LOG_I(TAG, "scan: %u tracks in %s", (unsigned)playlist->count, MP3_PLAYER_DATA_DIR);
    return playlist->count;
}

bool mp3_storage_track_path(
    const Mp3Playlist* playlist,
    int32_t index,
    char* out_path,
    size_t out_size) {
    if(index < 0 || (size_t)index >= playlist->count) return false;
    snprintf(
        out_path,
        out_size,
        "%s/%s",
        MP3_PLAYER_DATA_DIR,
        playlist->tracks[index].name);
    return true;
}
