#ifndef MP3_STORAGE_H
#define MP3_STORAGE_H

#include "mp3_player.h"
#include <storage/storage.h>

/* Allocate the playlist's track array (PSRAM, MP3_PLAYER_MAX_TRACKS slots). */
bool mp3_storage_playlist_alloc(Mp3Playlist* playlist);

/* Free the playlist's track array. */
void mp3_storage_playlist_free(Mp3Playlist* playlist);

/* Ensure MP3_PLAYER_DATA_DIR exists, then scan it non-recursively for *.mp3
 * filenames and fill the playlist (sorted is not guaranteed). Returns the
 * number of tracks added (capped at MP3_PLAYER_MAX_TRACKS). */
size_t mp3_storage_scan(Storage* storage, Mp3Playlist* playlist);

/* Build the full path for the given track index (e.g. /ext/apps_data/mp3_player/song.mp3).
 * out_path must hold at least 256 bytes. Returns false on bad index. */
bool mp3_storage_track_path(const Mp3Playlist* playlist, int32_t index, char* out_path, size_t out_size);

#endif /* MP3_STORAGE_H */
