/* id_sd.cpp — Sound-Driver-Layer für Wolf3D auf ESP32 / T-Embed.
 *
 * Originaler Wolf4SDL-Code (mit OPL3-Music + SDL_mixer Digi) liegt als
 * id_sd.cpp.full daneben. Hier eine schlanke Implementation, die nur
 * Digital-SFX (Sound Blaster Modus) macht — Music + PC-Speaker bleiben
 * stub. Output via wolf3d_i2s.cpp (I2S → MAX98357A).
 *
 * PCM-Format in Wolf3D-VSWAP: unsigned 8-bit, mono, 7000 Hz.
 * Mixer: 8 Channels, Q16.16 phase stepping (resampling 7000→22050 Hz).
 */

#include "wl_def.h"
#include "../wolf3d_i2s.h"

#include <string.h>
#include <stdlib.h>

#include <furi.h>

#define TAG "Wolf3D-SD"

#define ORIGSAMPLERATE  7000
#define NUM_SFX_CHANNELS 8

/* ---- Wolf-Globals ---- */
globalsoundpos channelSoundPos[NUM_SFX_CHANNELS];
boolean        AdLibPresent;
boolean        SoundBlasterPresent = true;
boolean        SoundSourcePresent;
SDMode         SoundMode = sdm_Off;
SDSMode        DigiMode  = sds_Off;
SMMode         MusicMode = smm_Off;
int            DigiMap[LASTSOUND];
/* DigiChannel ist indexed via digi-list-index (= DigiMap[soundname]), nicht
 * via Channel-Slot. InitDigiMap (wl_main.cpp:1040) schreibt bis ~70 weit —
 * mit Größe NUM_SFX_CHANNELS (8) hatten wir Out-of-bounds-Schreiben in
 * Memory hinter dem Array → korrumpierte SfxChannels → falsche Sounds. */
int            DigiChannel[LASTSOUND];

/* DigiList wird beim ersten SD_PrepareSound aus VSWAPs SoundInfoPage gelesen. */
struct DigiInfo { int startpage; int length; };
static DigiInfo* s_digi_list = nullptr;
static int       s_num_digi  = 0;

/* ---- Mixer-State ---- */
struct SfxChannel {
    const uint8_t* data;          /* PSRAM-allocated u8 PCM */
    uint32_t       length;        /* samples */
    uint32_t       position_q16;  /* Q16.16 phase */
    uint32_t       step_q16;      /* native_rate << 16 / I2S_SAMPLE_RATE */
    int            vol;           /* 0..127 */
    int            handle;        /* nonzero = active */
};
static SfxChannel s_chan[NUM_SFX_CHANNELS];
static int        s_next_handle = 1;
static volatile bool s_sfx_enabled = false;
static FuriMutex* s_sfx_mutex = nullptr;

extern "C++" {

/* SetupDigi: einmalig nach PM_Startup. SoundInfoPage am Ende der VSWAP
 * liefert pro Sound (startpage, length-low16). Page-Sizes summieren wir
 * für length (16-bit upper bits). */
static void SetupDigiList() {
    if(s_digi_list) return;
    word* soundInfoPage = (word*)(void*)PM_GetPage(ChunksInFile - 1);
    s_num_digi = (int)PM_GetPageSize(ChunksInFile - 1) / 4;

    s_digi_list = (DigiInfo*)heap_caps_calloc(
        s_num_digi, sizeof(DigiInfo), MALLOC_CAP_INTERNAL);
    if(!s_digi_list) { s_num_digi = 0; return; }

    for(int i = 0; i < s_num_digi; i++) {
        s_digi_list[i].startpage = soundInfoPage[i * 2];
        if(s_digi_list[i].startpage >= ChunksInFile - 1) {
            s_num_digi = i;
            break;
        }
        int lastPage;
        if(i < s_num_digi - 1) {
            lastPage = soundInfoPage[i * 2 + 2];
            if(lastPage == 0 || lastPage + PMSoundStart > ChunksInFile - 1)
                lastPage = ChunksInFile - 1;
            else
                lastPage += PMSoundStart;
        } else {
            lastPage = ChunksInFile - 1;
        }
        int size = 0;
        for(int page = PMSoundStart + s_digi_list[i].startpage; page < lastPage; page++)
            size += PM_GetPageSize(page);
        if(lastPage == ChunksInFile - 1 && PMSoundInfoPagePadded) size--;
        if((size & 0xffff0000) != 0 && (size & 0xffff) < soundInfoPage[i * 2 + 1])
            size -= 0x10000;
        size = (size & 0xffff0000) | soundInfoPage[i * 2 + 1];
        s_digi_list[i].length = size;
    }
    FURI_LOG_I(TAG, "SetupDigiList: %d sounds", s_num_digi);
}

/* ---- Public Wolf-API ---- */

void SD_Startup(void) {
    if(!s_sfx_mutex) s_sfx_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    memset(s_chan, 0, sizeof(s_chan));
    for(int i = 0; i < LASTSOUND; i++) {
        DigiMap[i] = -1;
        DigiChannel[i] = -1;
    }

    if(wolf3d_i2s_init()) {
        s_sfx_enabled = true;
        FURI_LOG_I(TAG, "SD_Startup: I2S OK, SFX enabled");
    } else {
        FURI_LOG_E(TAG, "SD_Startup: I2S init failed, SFX disabled");
    }
}

void SD_Shutdown(void) {
    s_sfx_enabled = false;
    /* I2S bleibt aktiv bis esp_restart() — wolf3d_i2s_deinit() könnte hier
     * gerufen werden, aber wir reboot eh beim Verlassen. */
}

void SD_StopSound(void) {
    if(!s_sfx_mutex) return;
    if(furi_mutex_acquire(s_sfx_mutex, 50) != FuriStatusOk) return;
    for(int i = 0; i < NUM_SFX_CHANNELS; i++) {
        s_chan[i].handle = 0;
        s_chan[i].data = nullptr;
    }
    furi_mutex_release(s_sfx_mutex);
}

void SD_WaitSoundDone(void) {}

int SD_GetChannelForDigi(int which) {
    /* Wolf3D ordnet manchen Sound-Indizes feste Channels zu (player weapon
     * = channel 0, boss weapon = channel 1) — in DigiChannel[] gesetzt von
     * InitDigiMap. -1 = freier Slot suchen. */
    if(which >= 0 && which < LASTSOUND && DigiChannel[which] != -1) {
        int ch = DigiChannel[which];
        if(ch >= 0 && ch < NUM_SFX_CHANNELS) return ch;
    }
    for(int i = 0; i < NUM_SFX_CHANNELS; i++)
        if(s_chan[i].handle == 0) return i;
    return 0;
}

void SD_PositionSound(int /*l*/, int /*r*/) {}
void SD_SetPosition(int /*ch*/, int /*l*/, int /*r*/) {}

void SD_PrepareSound(int which) {
    if(!s_digi_list) SetupDigiList();
    if(which < 0 || which >= s_num_digi) return;
    /* Lazy: Daten werden erst beim Play gelesen — PMPages lebt eh in PSRAM. */
}

int SD_PlayDigitized(word which, int /*leftpos*/, int /*rightpos*/) {
    if(!s_sfx_enabled || !s_sfx_mutex) return 0;
    if(!s_digi_list) SetupDigiList();
    if((int)which >= s_num_digi) return 0;

    const uint8_t* pcm = (const uint8_t*)PM_GetSound(s_digi_list[which].startpage);
    uint32_t len = (uint32_t)s_digi_list[which].length;
    if(!pcm || !len) return 0;

    if(furi_mutex_acquire(s_sfx_mutex, 10) != FuriStatusOk) return 0;
    int ch = SD_GetChannelForDigi((int)which);

    s_chan[ch].data         = pcm;
    s_chan[ch].length       = len;
    s_chan[ch].position_q16 = 0;
    s_chan[ch].step_q16     = ((uint32_t)ORIGSAMPLERATE << 16) / WOLF3D_I2S_SAMPLE_RATE;
    s_chan[ch].vol          = 100;
    int h = s_next_handle++;
    if(s_next_handle == 0) s_next_handle = 1;
    s_chan[ch].handle = h;

    furi_mutex_release(s_sfx_mutex);
    return ch + 1;  /* nonzero */
}

void SD_StopDigitized(void) { SD_StopSound(); }

boolean SD_PlaySound(soundnames sound) {
    if(!s_sfx_enabled) return false;
    if(DigiMode == sds_Off) return false;
    int idx = DigiMap[sound];
    if(idx < 0) return false;
    return SD_PlayDigitized((word)idx, 0, 0) != 0;
}

word SD_SoundPlaying(void) {
    for(int i = 0; i < NUM_SFX_CHANNELS; i++) if(s_chan[i].handle != 0) return 1;
    return 0;
}

void    SD_StartMusic(int) {}
void    SD_ContinueMusic(int, int) {}
void    SD_MusicOn(void) {}
int     SD_MusicOff(void) { return 0; }
void    SD_FadeOutMusic(void) {}
boolean SD_MusicPlaying(void) { return false; }

boolean SD_SetSoundMode(SDMode mode) {
    SoundMode = mode;
    return true;
}
boolean SD_SetMusicMode(SMMode mode) {
    MusicMode = mode;
    return true;
}
void SD_SetDigiDevice(SDSMode mode) {
    DigiMode = mode;
}

} /* extern "C++" */

/* ---- I2S-Hook (gerufen vom writer-thread) ---- */

extern "C" int32_t wolf3d_sfx_next_sample(void) {
    if(!s_sfx_enabled) return 0;
    int32_t mix = 0;
    /* Mutex-frei für maximalen Durchsatz; der writer hat höchste Prio,
     * Wolf-Game-Thread kann höchstens den `handle` zwischenzeitlich auf 0
     * setzen — ist atomic-int auf xtensa und unkritisch. */
    for(int i = 0; i < NUM_SFX_CHANNELS; i++) {
        SfxChannel* c = &s_chan[i];
        if(c->handle == 0 || !c->data) continue;
        uint32_t pos = c->position_q16 >> 16;
        if(pos >= c->length) {
            c->handle = 0;
            c->data = nullptr;
            continue;
        }
        int32_t s = (int32_t)(((int)c->data[pos] - 128) * 256);
        s = (s * c->vol) >> 7;
        mix += s;
        c->position_q16 += c->step_q16;
    }
    return mix;
}
