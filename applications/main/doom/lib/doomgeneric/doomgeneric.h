#ifndef DOOM_GENERIC
#define DOOM_GENERIC

#include <stdlib.h>
#include <stdint.h>

/* Hardgekodet auf Doom-Original 320x200 für den ESP32-Port. Vorher kamen
 * die Defines via application.fam-cdefines, aber buildFap.sh wertet diese
 * (noch) nicht aus → Default 640x400 schlug bis ins Rendering durch. */
#ifndef DOOMGENERIC_RESX
#define DOOMGENERIC_RESX 320
#endif  // DOOMGENERIC_RESX

#ifndef DOOMGENERIC_RESY
#define DOOMGENERIC_RESY 200
#endif  // DOOMGENERIC_RESY


#ifdef CMAP256

typedef uint8_t pixel_t;

#else  // CMAP256

typedef uint32_t pixel_t;

#endif  // CMAP256


extern pixel_t* DG_ScreenBuffer;

#ifdef __cplusplus
extern "C" {
#endif

void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick();


//Implement below functions for your platform
void DG_Init();
void DG_DrawFrame();
void DG_SleepMs(uint32_t ms);
uint32_t DG_GetTicksMs();
int DG_GetKey(int* pressed, unsigned char* key);
void DG_SetWindowTitle(const char * title);

#ifdef __cplusplus
}
#endif

#endif //DOOM_GENERIC
