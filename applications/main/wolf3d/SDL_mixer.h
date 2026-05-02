/* SDL_mixer.h shim — Wolf4SDLs SDL_mixer-Calls werden durch unsere
 * eigene Audio-Pipeline ersetzt (Stage 3). Hier Stubs. */
#ifndef WOLF3D_SDL_MIXER_SHIM_H
#define WOLF3D_SDL_MIXER_SHIM_H

#include "SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Mix_Chunk { int dummy; } Mix_Chunk;
typedef struct Mix_Music { int dummy; } Mix_Music;

#define MIX_DEFAULT_FORMAT 0x8010
#define MIX_MAX_VOLUME     128
#define MIX_CHANNELS       8
#define MIX_INIT_FLAC      0x00000001
#define MIX_INIT_MOD       0x00000002
#define MIX_INIT_MP3       0x00000008
#define MIX_INIT_OGG       0x00000010

static inline int          Mix_OpenAudio(int, uint16_t, int, int) { return 0; }
static inline void         Mix_CloseAudio(void) {}
static inline int          Mix_Init(int) { return 0; }
static inline void         Mix_Quit(void) {}
static inline int          Mix_AllocateChannels(int n) { return n; }
static inline Mix_Chunk*   Mix_LoadWAV(const char*) { return NULL; }
static inline void         Mix_FreeChunk(Mix_Chunk*) {}
static inline int          Mix_PlayChannel(int, Mix_Chunk*, int) { return -1; }
static inline int          Mix_PlayChannelTimed(int, Mix_Chunk*, int, int) { return -1; }
static inline int          Mix_HaltChannel(int) { return 0; }
static inline int          Mix_Volume(int, int) { return 0; }
static inline int          Mix_VolumeChunk(Mix_Chunk*, int) { return 0; }
static inline int          Mix_Playing(int) { return 0; }
static inline int          Mix_FadeOutChannel(int, int) { return 0; }
static inline int          Mix_GroupChannel(int, int) { return 0; }
static inline int          Mix_GroupAvailable(int) { return -1; }
static inline int          Mix_ReserveChannels(int) { return 0; }
static inline void         Mix_HookMusic(void(*)(void*, uint8_t*, int), void*) {}
static inline int          Mix_QuerySpec(int*, uint16_t*, int*) { return 0; }
static inline const char*  Mix_GetError(void) { return ""; }

#ifdef __cplusplus
}
#endif

#endif
