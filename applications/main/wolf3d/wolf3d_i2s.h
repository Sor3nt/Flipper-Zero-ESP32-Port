/* wolf3d_i2s.h — I2S-Backend für Wolf3D-SFX auf T-Embed (MAX98357A).
 * Eng angelehnt an applications/main/doom/lib/doomgeneric/doom_i2s.h. */

#ifndef WOLF3D_I2S_H
#define WOLF3D_I2S_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WOLF3D_I2S_SAMPLE_RATE 22050

bool wolf3d_i2s_init(void);
void wolf3d_i2s_deinit(void);

/* Hook implementiert in id_sd.cpp — liefert mixed int32 sample (pre-clip). */
int32_t wolf3d_sfx_next_sample(void);

#ifdef __cplusplus
}
#endif
#endif
