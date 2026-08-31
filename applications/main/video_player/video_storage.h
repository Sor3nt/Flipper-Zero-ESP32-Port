#ifndef VIDEO_STORAGE_H
#define VIDEO_STORAGE_H

#include "video_player.h"
#include <storage/storage.h>

/* Allocate the file-list array (PSRAM, VIDEO_PLAYER_MAX_FILES slots). */
bool video_storage_list_alloc(VideoList* list);

/* Free the file-list array. */
void video_storage_list_free(VideoList* list);

/* Ensure VIDEO_PLAYER_DATA_DIR exists, then scan it non-recursively for video
 * files and fill the list. Returns the number added (capped). */
size_t video_storage_scan(Storage* storage, VideoList* list);

/* Build the full SD path for the given index. out_path >= 256 bytes. */
bool video_storage_file_path(const VideoList* list, int32_t index, char* out_path, size_t out_size);

#endif /* VIDEO_STORAGE_H */
