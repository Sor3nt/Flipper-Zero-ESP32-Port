/**
 * @file i_esp32_sfx.c
 * Sound-effect backend for the ESP32 Doom port.
 *
 * - Loads raw Doom SFX lumps from the WAD on demand (skipped 8-byte
 *   DMX header + 16-byte trailing padding), converts u8 → s16 once,
 *   caches the converted buffer in PSRAM via sfxinfo_t::driver_data.
 * - Mixes up to 8 concurrent channels, applies vol/sep, resamples
 *   from each sfx's native rate (usually 11025 Hz) to DOOM_I2S_SAMPLE_RATE
 *   with Q16.16 linear phase stepping.
 * - The writer thread in doom_i2s.c pulls one mixed int32 sample at a
 *   time through doom_sfx_next_sample().
 */

#include <string.h>
#include <stdlib.h>

#include "doomtype.h"
#include "i_sound.h"
#include "i_system.h"
#include "w_wad.h"
#include "z_zone.h"

#include "doom_i2s.h"

#include <furi.h>
#include <esp_heap_caps.h>

#ifndef arrlen
#define arrlen(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define NUM_SFX_CHANNELS 8

typedef struct {
    int16_t* data;     /* PSRAM-allocated, signed 16-bit mono */
    size_t   length;   /* samples */
    uint32_t step_q16; /* native_rate << 16 / DOOM_I2S_SAMPLE_RATE */
    int      lumpnum;
} sfx_sample_t;

typedef struct {
    sfx_sample_t* sample;
    uint32_t      position_q16;
    int           vol;     /* 0..127 */
    int           sep;     /* 0..254 (ignored — we output mono) */
    int           handle;  /* return value of StartSound, 0 if free */
} sfx_channel_t;

static sfx_channel_t channels[NUM_SFX_CHANNELS];
static int           next_handle = 1;
static volatile bool sfx_enabled = false;

/* -------- Lump loader -------- */

static sfx_sample_t* sfx_load_lump(sfxinfo_t* sfx) {
    if(sfx->driver_data) return (sfx_sample_t*)sfx->driver_data;
    if(sfx->lumpnum < 0) return NULL;

    byte* data = W_CacheLumpNum(sfx->lumpnum, PU_STATIC);
    int lump_len = W_LumpLength(sfx->lumpnum);
    if(!data || lump_len < 8) return NULL;

    /* DMX sound header:
     *   0-1: format tag (always 3 for PCM)
     *   2-3: sample rate (little-endian)
     *   4-7: number of samples (little-endian)
     *   8+ : unsigned 8-bit PCM data, followed by 16 bytes of padding
     */
    uint16_t rate = (uint16_t)(data[2] | (data[3] << 8));
    uint32_t nsamples = (uint32_t)(data[4] | (data[5] << 8) |
                                   (data[6] << 16) | (data[7] << 24));
    if(rate < 8000 || rate > 48000) rate = 11025;
    if(nsamples + 8 > (uint32_t)lump_len) nsamples = lump_len - 8;
    if(nsamples > 32) nsamples -= 16; /* strip trailing padding */

    sfx_sample_t* s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_SPIRAM);
    if(!s) return NULL;
    s->data = heap_caps_malloc(nsamples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if(!s->data) { heap_caps_free(s); return NULL; }

    const byte* src = data + 8;
    for(uint32_t i = 0; i < nsamples; i++) {
        /* u8 → s16: (x - 128) * 256 → range ±32768, preserves loudness */
        s->data[i] = (int16_t)((int)src[i] - 128) * 256;
    }
    s->length = nsamples;
    s->step_q16 = ((uint32_t)rate << 16) / DOOM_I2S_SAMPLE_RATE;
    s->lumpnum = sfx->lumpnum;

    sfx->driver_data = s;
    return s;
}

/* -------- sound_module_t callbacks -------- */

static boolean I_ESP32_SFX_Init(boolean use_sfx_prefix) {
    (void)use_sfx_prefix;
    memset(channels, 0, sizeof(channels));
    sfx_enabled = true;
    return true;
}

static void I_ESP32_SFX_Shutdown(void) {
    sfx_enabled = false;
    memset(channels, 0, sizeof(channels));
}

static int I_ESP32_SFX_GetSfxLumpNum(sfxinfo_t* sfx) {
    char namebuf[9];
    /* Vanilla Doom always prefixes SFX lump names with "ds". */
    snprintf(namebuf, sizeof(namebuf), "ds%s", sfx->name);
    return W_CheckNumForName(namebuf);
}

static void I_ESP32_SFX_Update(void) { /* mixing happens in the I2S writer */ }

static void I_ESP32_SFX_UpdateSoundParams(int channel, int vol, int sep) {
    if(channel < 0 || channel >= NUM_SFX_CHANNELS) return;
    channels[channel].vol = vol;
    channels[channel].sep = sep;
}

static int I_ESP32_SFX_StartSound(sfxinfo_t* sfx, int channel, int vol, int sep) {
    if(!sfx_enabled) return -1;
    if(channel < 0 || channel >= NUM_SFX_CHANNELS) return -1;

    sfx_sample_t* s = sfx_load_lump(sfx);
    if(!s) return -1;

    /* A writer thread is reading from this slot; do not allocate or free
     * anything — just overwrite. The atomic assignment is OK because the
     * writer only reads the channel if handle != 0 (checked last). */
    channels[channel].sample = s;
    channels[channel].position_q16 = 0;
    channels[channel].vol = vol;
    channels[channel].sep = sep;
    int h = next_handle++;
    if(next_handle == 0) next_handle = 1;
    channels[channel].handle = h;
    return h;
}

static void I_ESP32_SFX_StopSound(int channel) {
    if(channel < 0 || channel >= NUM_SFX_CHANNELS) return;
    channels[channel].handle = 0;
    channels[channel].sample = NULL;
}

static boolean I_ESP32_SFX_SoundIsPlaying(int channel) {
    if(channel < 0 || channel >= NUM_SFX_CHANNELS) return false;
    return channels[channel].handle != 0 && channels[channel].sample != NULL;
}

static void I_ESP32_SFX_CacheSounds(sfxinfo_t* sounds, int num_sounds) {
    (void)sounds; (void)num_sounds;
    /* Lazy loading — sounds are brought in at first StartSound. */
}

/* -------- Mixer hook used by doom_i2s writer thread -------- */

int32_t doom_sfx_next_sample(void) {
    if(!sfx_enabled) return 0;
    int32_t mix = 0;
    for(int i = 0; i < NUM_SFX_CHANNELS; i++) {
        sfx_channel_t* c = &channels[i];
        if(c->handle == 0 || !c->sample) continue;

        uint32_t pos = c->position_q16 >> 16;
        if(pos >= c->sample->length) {
            c->handle = 0;
            c->sample = NULL;
            continue;
        }

        int32_t s = c->sample->data[pos];
        /* Apply volume (0..127) with a Q7 multiply. */
        s = (s * c->vol) >> 7;
        mix += s;

        c->position_q16 += c->sample->step_q16;
    }
    return mix;
}

/* -------- Module registration -------- */

static snddevice_t esp32_sfx_devices[] = {
    SNDDEVICE_SB,
};

sound_module_t DG_sound_module = {
    esp32_sfx_devices,
    arrlen(esp32_sfx_devices),
    I_ESP32_SFX_Init,
    I_ESP32_SFX_Shutdown,
    I_ESP32_SFX_GetSfxLumpNum,
    I_ESP32_SFX_Update,
    I_ESP32_SFX_UpdateSoundParams,
    I_ESP32_SFX_StartSound,
    I_ESP32_SFX_StopSound,
    I_ESP32_SFX_SoundIsPlaying,
    I_ESP32_SFX_CacheSounds,
};
