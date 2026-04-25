/**
 * @file i_esp32_music.c
 * Music backend stub for the ESP32 Doom port.
 *
 * TODO: Wire up a real OPL2/3 synth (Chocolate-Doom's opl/ + i_oplmusic.c
 * pathway) so the Doom level music (MUS lumps) actually plays through
 * doom_music_fill_block(). See plan file for the design.
 *
 * Until then, every music_module_t callback is a harmless no-op so
 * s_sound.c can call RegisterSong / PlaySong without crashing.
 */

#include <string.h>

#include "doomtype.h"
#include "i_sound.h"

#include "doom_i2s.h"

#ifndef arrlen
#define arrlen(x) (sizeof(x) / sizeof((x)[0]))
#endif

static boolean I_ESP32_Music_Init(void) { return true; }
static void    I_ESP32_Music_Shutdown(void) {}
static void    I_ESP32_Music_SetMusicVolume(int v) { (void)v; }
static void    I_ESP32_Music_PauseMusic(void) {}
static void    I_ESP32_Music_ResumeMusic(void) {}
static void*   I_ESP32_Music_RegisterSong(void* data, int len) {
    (void)data; (void)len;
    /* Return a non-NULL pointer so s_sound.c treats the handle as valid
     * and does not fall back to its own silent shim. */
    static int dummy;
    return &dummy;
}
static void    I_ESP32_Music_UnRegisterSong(void* h) { (void)h; }
static void    I_ESP32_Music_PlaySong(void* h, boolean looping) { (void)h; (void)looping; }
static void    I_ESP32_Music_StopSong(void) {}
static boolean I_ESP32_Music_MusicIsPlaying(void) { return false; }
static void    I_ESP32_Music_Poll(void) {}

static snddevice_t esp32_music_devices[] = {
    SNDDEVICE_GENMIDI,
    SNDDEVICE_ADLIB,
    SNDDEVICE_SB,
};

music_module_t DG_music_module = {
    esp32_music_devices,
    arrlen(esp32_music_devices),
    I_ESP32_Music_Init,
    I_ESP32_Music_Shutdown,
    I_ESP32_Music_SetMusicVolume,
    I_ESP32_Music_PauseMusic,
    I_ESP32_Music_ResumeMusic,
    I_ESP32_Music_RegisterSong,
    I_ESP32_Music_UnRegisterSong,
    I_ESP32_Music_PlaySong,
    I_ESP32_Music_StopSong,
    I_ESP32_Music_MusicIsPlaying,
    I_ESP32_Music_Poll,
};

/* Weak symbol used by the I2S writer thread. Returns silence while the
 * music module is a stub — when a real OPL synth is wired in, this
 * function will render the active song's samples into `out`. */
void doom_music_fill_block(int32_t* out, size_t frames) {
    (void)out; (void)frames;
    /* Already memset to 0 by the writer before calling. */
}
