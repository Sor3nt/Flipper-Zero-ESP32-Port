/* SPDX-License-Identifier: MIT */
#pragma once
#include <gui/gui.h>
#include <stdbool.h>
#include <stddef.h>

bool video_player_is_local(const char* path);
/* Runs on the app thread; directly subscribes to input while GUI is held.
 * Releases display, input, audio and storage on every exit path. */
bool video_player_run(Gui* gui, const char* path, uint8_t* volume,
                      char* error, size_t error_size);
