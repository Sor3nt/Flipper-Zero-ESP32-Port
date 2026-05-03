/* wolf3d_glue.h — Schnittstelle zwischen wolf3d_app.c (C) und der
 * SDL-Shim / Display-/Input-Pipeline (C++). */

#ifndef WOLF3D_GLUE_H
#define WOLF3D_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

struct Gui;
typedef struct Gui Gui;
struct FuriPubSub;
typedef struct FuriPubSub FuriPubSub;

/* Lifecycle */
void  wolf3d_glue_setup(Gui* gui, FuriPubSub* input_events);
void  wolf3d_glue_teardown(void);
void  wolf3d_glue_show_splash(void);

/* Wolf4SDLs Quit() macht longjmp() hierher zurück, statt exit() zu rufen. */
int   wolf3d_glue_set_quit_jmp(void);
void  wolf3d_glue_do_quit(void) __attribute__((noreturn));

/* Vom Renderer aufgerufen: 8-bit indexed Framebuffer (320x200) auf das
 * Panel kippen. Palette wird intern vorgehalten. */
void  wolf3d_glue_present_indexed(const uint8_t* pixels,
                                  int width, int height, int pitch);
void  wolf3d_glue_set_palette(const uint8_t* rgb_triplets, int first, int count);

/* Time / mutex helpers used by sdl_glue.cpp */
uint32_t wolf3d_glue_ticks_ms(void);
void     wolf3d_glue_delay_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif
