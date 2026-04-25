/**
 * @file doom_i2s.h
 * Low-level I2S output for the Doom ESP32 port. Owns I2S_NUM_0 exclusively
 * (the normal furi_hal_speaker is torn down before doom_i2s_init()).
 *
 * The writer thread pulls samples from two source hooks per output frame:
 *   - doom_sfx_next_sample()  — one int32 sample from the SFX mixer
 *                               (implemented in i_esp32_sfx.c)
 *   - doom_music_fill_block() — a block of int32 samples from the music
 *                               backend (stub today, OPL synth later)
 * Both are weak symbols so the module links even before either mixer is
 * compiled in. Results are clipped to int16 and duplicated to L/R (the
 * MAX98357A sums them internally).
 */

#ifndef DOOM_I2S_H
#define DOOM_I2S_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOOM_I2S_SAMPLE_RATE 22050

/** Initialise I2S_NUM_0 in standard mode for the MAX98357A speaker on
 * the T-Embed. Must be called *after* furi_hal_speaker_deinit() so we
 * get exclusive ownership of the controller. Returns true on success. */
bool doom_i2s_init(void);

/** Stop the writer thread and tear down I2S. Unused today because the
 * app reboots on exit; provided for completeness. */
void doom_i2s_deinit(void);

/* -- Hooks consumed by the writer thread. Both have weak default no-op
 *    implementations in doom_i2s.c, overridden by the SFX/music modules. */

/** Return the next mixed SFX sample (int32, pre-clip). Called once per
 * output frame from the writer thread. */
int32_t doom_sfx_next_sample(void);

/** Render `frames` int32 samples of music into `out`. Called once per
 * writer chunk. `out` is pre-zeroed by the writer. */
void    doom_music_fill_block(int32_t* out, size_t frames);

#ifdef __cplusplus
}
#endif

#endif /* DOOM_I2S_H */
