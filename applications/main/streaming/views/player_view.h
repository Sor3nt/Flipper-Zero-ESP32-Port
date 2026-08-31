#pragma once

#include <gui/view.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct PlayerView PlayerView;

/* Events the player view raises from its input handler. The owning scene maps
 * these onto its own SceneManager custom events (keeps the view decoupled). */
typedef enum {
    PlayerViewEventPlayPause,
    PlayerViewEventSeekBack,
    PlayerViewEventSeekForward,
} PlayerViewEvent;

typedef void (*PlayerViewCallback)(PlayerViewEvent event, void* context);

PlayerView* player_view_alloc(void);
void player_view_free(PlayerView* v);
View* player_view_get_view(PlayerView* v);
void player_view_set_callback(PlayerView* v, PlayerViewCallback cb, void* ctx);

/* Refresh the now-playing display. state: 0=idle/stopped, 1=playing, 2=paused.
 * seekable enables the ◄ ► seek hints (Cast/DLNA only). */
void player_view_update(
    PlayerView* v,
    const char* title,
    const char* target,
    uint32_t elapsed_ms,
    uint32_t duration_ms,
    uint8_t state,
    bool seekable,
    bool buffering);
