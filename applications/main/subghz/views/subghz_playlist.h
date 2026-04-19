#pragma once

#include <gui/view.h>
#include "../helpers/subghz_types.h"
#include "../helpers/subghz_custom_event.h"

typedef struct SubGhzPlaylist SubGhzPlaylist;

typedef void (*SubGhzPlaylistCallback)(SubGhzCustomEvent event, void* context);

SubGhzPlaylist* subghz_playlist_alloc(void);
void subghz_playlist_free(SubGhzPlaylist* instance);

View* subghz_playlist_get_view(SubGhzPlaylist* instance);

void subghz_playlist_set_callback(
    SubGhzPlaylist* instance,
    SubGhzPlaylistCallback callback,
    void* context);

void subghz_playlist_set_file_path(SubGhzPlaylist* instance, const char* file_path);
void subghz_playlist_start(SubGhzPlaylist* instance);
void subghz_playlist_stop(SubGhzPlaylist* instance);
