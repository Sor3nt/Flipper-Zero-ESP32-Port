/* SDL.h — Mini-Shim für den Wolf3D-Port auf ESP32 / T-Embed.
 *
 * Wolf4SDL bezieht alles aus `<SDL.h>` (Surface, Palette, Renderer, Mutex,
 * Audio, Events, Joystick, Timer). Wir liefern hier nur das, was Wolf4SDL
 * tatsächlich verwendet — alles andere ist Stub oder fehlt.
 *
 * Pipeline-Modell:
 *   Game zeichnet auf SDL_Surface->pixels (8-bit indexed).
 *   SDL_BlitSurface / SDL_FillRect arbeiten in Software auf den 8-bit-Bytes.
 *   Am Frame-Ende ruft Wolf4SDL SDL_UpdateTexture + SDL_RenderCopy +
 *   SDL_RenderPresent — letzteres triggert unsere echte Pipeline
 *   (8-bit + Palette → RGB565 stripes → ST7789 panel).
 *
 * Trick: `#define main wolf_main` lässt Wolf4SDLs `int main(...)` aus
 * wl_main.cpp zur normalen Funktion `wolf_main` werden, aufrufbar aus
 * wolf3d_app.c. Identisches Pattern wie SDL2-`#define main SDL_main`.
 */

#ifndef WOLF3D_SDL_SHIM_H
#define WOLF3D_SDL_SHIM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* ESP-IDFs newlib + libstdc++ kombinieren <math.h> in C++-Mode unsauber:
 * <cmath> ist defekt, und <math.h>-Funktionen sind im global scope nicht
 * verlinkt. Wir deklarieren die von Wolf4SDL gebrauchten direkt. */
double atan(double);
double atan2(double, double);
float  atan2f(float, float);
double tan(double);
double sin(double);
double cos(double);
double sqrt(double);
double ceil(double);
double floor(double);
double fabs(double);

/* SDL1-Style Typen (Wolf4SDL benutzt sie noch). */
typedef uint8_t  Uint8;
typedef int8_t   Sint8;
typedef uint16_t Uint16;
typedef int16_t  Sint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;

/* Wolf4SDLs `int main(int, char**)` wird zu `wolf_main` umbenannt. Vorab
 * als extern "C" deklariert, damit wl_main.cpps Definition C-linkage erbt
 * und nicht C++-mangled ausgegeben wird. */
#ifdef __cplusplus
extern "C" int wolf_main(int argc, char* argv[]);
#endif
#define main wolf_main

/* ---- Initialisierungs-Flags ---- */
#define SDL_INIT_VIDEO    0x00000020u
#define SDL_INIT_AUDIO    0x00000010u
#define SDL_INIT_JOYSTICK 0x00000200u

#define SDL_TRUE  1
#define SDL_FALSE 0
#define SDL_ENABLE 1

/* ---- Window / Renderer / Texture (Stubs) ---- */
typedef struct SDL_Window   SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture  SDL_Texture;

#define SDL_WINDOWPOS_CENTERED          0
#define SDL_WINDOW_ALLOW_HIGHDPI        0x00002000u
#define SDL_WINDOW_RESIZABLE            0x00000020u
#define SDL_WINDOW_FULLSCREEN_DESKTOP   0x00001001u
#define SDL_RENDERER_PRESENTVSYNC       0x00000004u
#define SDL_RENDERER_SOFTWARE           0x00000001u
#define SDL_TEXTUREACCESS_STREAMING     1
#define SDL_HINT_RENDER_SCALE_QUALITY   "SDL_RENDER_SCALE_QUALITY"

/* ---- Color / Palette / Surface ---- */
typedef struct SDL_Color {
    uint8_t r, g, b, a;
} SDL_Color;

typedef struct SDL_Palette {
    int        ncolors;
    SDL_Color  colors[256];
    uint32_t   version;
    int        refcount;
} SDL_Palette;

typedef struct SDL_PixelFormat {
    uint32_t        format;
    SDL_Palette*    palette;
    uint8_t         BitsPerPixel;
    uint8_t         BytesPerPixel;
    uint8_t         padding[2];
    uint32_t        Rmask, Gmask, Bmask, Amask;
} SDL_PixelFormat;

typedef struct SDL_Rect {
    int x, y, w, h;
} SDL_Rect;

typedef struct SDL_Surface {
    uint32_t          flags;
    SDL_PixelFormat*  format;
    int               w, h;
    int               pitch;
    void*             pixels;
    void*             userdata;
    int               locked;
    void*             lock_data;
    SDL_Rect          clip_rect;
    void*             map;
    int               refcount;
} SDL_Surface;

#define SDL_MUSTLOCK(s) (0)

/* ---- Mutex ---- */
typedef struct SDL_mutex SDL_mutex;

/* ---- Audio ---- */
#define SDL_AUDIO_ALLOW_FREQUENCY_CHANGE 0x00000001u
typedef struct SDL_AudioCVT {
    int       needed;
    uint16_t  src_format;
    uint16_t  dst_format;
    double    rate_incr;
    uint8_t*  buf;
    int       len;
    int       len_cvt;
    int       len_mult;
    double    len_ratio;
    void*     filters[10];
    int       filter_index;
} SDL_AudioCVT;

/* ---- Joystick (Stubs — kein Joystick auf T-Embed) ---- */
typedef struct SDL_Joystick SDL_Joystick;
#define SDL_HAT_UP      0x01
#define SDL_HAT_RIGHT   0x02
#define SDL_HAT_DOWN    0x04
#define SDL_HAT_LEFT    0x08

/* ---- Events / Scancodes / Keymod ---- */
typedef int32_t SDL_Keymod;

typedef struct SDL_Keysym {
    int      scancode;
    int32_t  sym;
    uint16_t mod;
    uint32_t unused;
} SDL_Keysym;

#define SDL_SCANCODE_RETURN     40
#define SDL_SCANCODE_KP_ENTER   88

/* ---- SDLK_* Keycodes (SDL2-Pattern) ----
 * Wolf4SDL nutzt diese in id_in.h als sc_*-Defines (Tastatur-Mapping).
 * Reguläre Keys sind ASCII; Spezialkeys haben Bit 30 gesetzt + SDL2-Scancode. */
#define SDLK_SCANCODE_MASK         (1<<30)
#define SDL_SCANCODE_TO_KEYCODE(X) ((X) | SDLK_SCANCODE_MASK)

#define SDLK_UNKNOWN     0
#define SDLK_RETURN      '\r'
#define SDLK_ESCAPE      '\x1B'
#define SDLK_BACKSPACE   '\b'
#define SDLK_TAB         '\t'
#define SDLK_SPACE       ' '
#define SDLK_DELETE      '\x7F'

#define SDLK_0  '0'
#define SDLK_1  '1'
#define SDLK_2  '2'
#define SDLK_3  '3'
#define SDLK_4  '4'
#define SDLK_5  '5'
#define SDLK_6  '6'
#define SDLK_7  '7'
#define SDLK_8  '8'
#define SDLK_9  '9'

#define SDLK_a 'a'
#define SDLK_b 'b'
#define SDLK_c 'c'
#define SDLK_d 'd'
#define SDLK_e 'e'
#define SDLK_f 'f'
#define SDLK_g 'g'
#define SDLK_h 'h'
#define SDLK_i 'i'
#define SDLK_j 'j'
#define SDLK_k 'k'
#define SDLK_l 'l'
#define SDLK_m 'm'
#define SDLK_n 'n'
#define SDLK_o 'o'
#define SDLK_p 'p'
#define SDLK_q 'q'
#define SDLK_r 'r'
#define SDLK_s 's'
#define SDLK_t 't'
#define SDLK_u 'u'
#define SDLK_v 'v'
#define SDLK_w 'w'
#define SDLK_x 'x'
#define SDLK_y 'y'
#define SDLK_z 'z'

#define SDLK_F1   SDL_SCANCODE_TO_KEYCODE(58)
#define SDLK_F2   SDL_SCANCODE_TO_KEYCODE(59)
#define SDLK_F3   SDL_SCANCODE_TO_KEYCODE(60)
#define SDLK_F4   SDL_SCANCODE_TO_KEYCODE(61)
#define SDLK_F5   SDL_SCANCODE_TO_KEYCODE(62)
#define SDLK_F6   SDL_SCANCODE_TO_KEYCODE(63)
#define SDLK_F7   SDL_SCANCODE_TO_KEYCODE(64)
#define SDLK_F8   SDL_SCANCODE_TO_KEYCODE(65)
#define SDLK_F9   SDL_SCANCODE_TO_KEYCODE(66)
#define SDLK_F10  SDL_SCANCODE_TO_KEYCODE(67)
#define SDLK_F11  SDL_SCANCODE_TO_KEYCODE(68)
#define SDLK_F12  SDL_SCANCODE_TO_KEYCODE(69)

#define SDLK_INSERT     SDL_SCANCODE_TO_KEYCODE(73)
#define SDLK_HOME       SDL_SCANCODE_TO_KEYCODE(74)
#define SDLK_PAGEUP     SDL_SCANCODE_TO_KEYCODE(75)
#define SDLK_END        SDL_SCANCODE_TO_KEYCODE(77)
#define SDLK_PAGEDOWN   SDL_SCANCODE_TO_KEYCODE(78)

#define SDLK_RIGHT      SDL_SCANCODE_TO_KEYCODE(79)
#define SDLK_LEFT       SDL_SCANCODE_TO_KEYCODE(80)
#define SDLK_DOWN       SDL_SCANCODE_TO_KEYCODE(81)
#define SDLK_UP         SDL_SCANCODE_TO_KEYCODE(82)

#define SDLK_LCTRL      SDL_SCANCODE_TO_KEYCODE(224)
#define SDLK_LSHIFT     SDL_SCANCODE_TO_KEYCODE(225)
#define SDLK_LALT       SDL_SCANCODE_TO_KEYCODE(226)
#define SDLK_RSHIFT     SDL_SCANCODE_TO_KEYCODE(229)
#define SDLK_RCTRL      SDL_SCANCODE_TO_KEYCODE(228)
#define SDLK_RALT       SDL_SCANCODE_TO_KEYCODE(230)

#define SDLK_CAPSLOCK   SDL_SCANCODE_TO_KEYCODE(57)
/* SDL1-Aliase, Wolf4SDL benutzt sie noch in id_in.h */
#define SDLK_SCROLLOCK  SDL_SCANCODE_TO_KEYCODE(71)
#define SDLK_PRINT      SDL_SCANCODE_TO_KEYCODE(70)
#define SDLK_PRINTSCREEN SDLK_PRINT

#define SDLK_KP_0       SDL_SCANCODE_TO_KEYCODE(98)
#define SDLK_KP_1       SDL_SCANCODE_TO_KEYCODE(89)
#define SDLK_KP_2       SDL_SCANCODE_TO_KEYCODE(90)
#define SDLK_KP_3       SDL_SCANCODE_TO_KEYCODE(91)
#define SDLK_KP_4       SDL_SCANCODE_TO_KEYCODE(92)
#define SDLK_KP_5       SDL_SCANCODE_TO_KEYCODE(93)
#define SDLK_KP_6       SDL_SCANCODE_TO_KEYCODE(94)
#define SDLK_KP_7       SDL_SCANCODE_TO_KEYCODE(95)
#define SDLK_KP_8       SDL_SCANCODE_TO_KEYCODE(96)
#define SDLK_KP_9       SDL_SCANCODE_TO_KEYCODE(97)
#define SDLK_KP_ENTER   SDL_SCANCODE_TO_KEYCODE(88)

#define SDLK_COMMA      ','
#define SDLK_PERIOD     '.'
#define SDLK_MINUS      '-'
#define SDLK_PLUS       '+'
#define SDLK_SLASH      '/'
#define SDLK_BACKSLASH  '\\'
#define SDLK_LEFTBRACKET  '['
#define SDLK_RIGHTBRACKET ']'
#define SDLK_SEMICOLON  ';'
#define SDLK_QUOTE      '\''
#define SDLK_BACKQUOTE  '`'
#define SDLK_EQUALS     '='
#define SDLK_PAUSE          SDL_SCANCODE_TO_KEYCODE(72)
#define SDLK_NUMLOCKCLEAR   SDL_SCANCODE_TO_KEYCODE(83)
#define SDLK_SCROLLLOCK     SDL_SCANCODE_TO_KEYCODE(71)
#define SDLK_LGUI           SDL_SCANCODE_TO_KEYCODE(227)
#define SDLK_RGUI           SDL_SCANCODE_TO_KEYCODE(231)
#define SDLK_F13            SDL_SCANCODE_TO_KEYCODE(104)
#define SDLK_F14            SDL_SCANCODE_TO_KEYCODE(105)
#define SDLK_F15            SDL_SCANCODE_TO_KEYCODE(106)
#define SDLK_F16            SDL_SCANCODE_TO_KEYCODE(107)
#define SDLK_F17            SDL_SCANCODE_TO_KEYCODE(108)
#define SDLK_F18            SDL_SCANCODE_TO_KEYCODE(109)
#define SDLK_F19            SDL_SCANCODE_TO_KEYCODE(110)
#define SDLK_F20            SDL_SCANCODE_TO_KEYCODE(111)
#define SDLK_F21            SDL_SCANCODE_TO_KEYCODE(112)
#define SDLK_F22            SDL_SCANCODE_TO_KEYCODE(113)
#define SDLK_F23            SDL_SCANCODE_TO_KEYCODE(114)
#define SDLK_F24            SDL_SCANCODE_TO_KEYCODE(115)

/* ASCII-symbols (Wolf4SDL erwartet sie als sc_-Mapping) */
#define SDLK_EXCLAIM        '!'
#define SDLK_QUOTEDBL       '"'
#define SDLK_HASH           '#'
#define SDLK_DOLLAR         '$'
#define SDLK_PERCENT        '%'
#define SDLK_AMPERSAND      '&'
#define SDLK_LEFTPAREN      '('
#define SDLK_RIGHTPAREN     ')'
#define SDLK_ASTERISK       '*'
#define SDLK_COLON          ':'
#define SDLK_LESS           '<'
#define SDLK_GREATER        '>'
#define SDLK_QUESTION       '?'
#define SDLK_AT             '@'
#define SDLK_CARET          '^'
#define SDLK_UNDERSCORE     '_'

/* ---- Keymod-Bits ---- */
#define KMOD_NONE       0x0000
#define KMOD_LSHIFT     0x0001
#define KMOD_RSHIFT     0x0002
#define KMOD_LCTRL      0x0040
#define KMOD_RCTRL      0x0080
#define KMOD_LALT       0x0100
#define KMOD_RALT       0x0200
#define KMOD_NUM        0x1000
#define KMOD_CAPS       0x2000
#define KMOD_SHIFT      (KMOD_LSHIFT|KMOD_RSHIFT)
#define KMOD_CTRL       (KMOD_LCTRL|KMOD_RCTRL)
#define KMOD_ALT        (KMOD_LALT|KMOD_RALT)

/* ---- Audio-Format-Konstanten ---- */
#define AUDIO_U8        0x0008
#define AUDIO_S8        0x8008
#define AUDIO_U16LSB    0x0010
#define AUDIO_S16LSB    0x8010
#define AUDIO_U16MSB    0x1010
#define AUDIO_S16MSB    0x9010
#define AUDIO_U16       AUDIO_U16LSB
#define AUDIO_S16       AUDIO_S16LSB
#define AUDIO_S16SYS    AUDIO_S16LSB
#define AUDIO_U16SYS    AUDIO_U16LSB

/* SDL Event types — wir liefern KEYDOWN/KEYUP/QUIT, alles andere wird
 * nicht erzeugt und ignoriert. */
#define SDL_QUIT                0x100
#define SDL_KEYDOWN             0x300
#define SDL_KEYUP               0x301
#define SDL_MOUSEBUTTONDOWN     0x401
#define SDL_MOUSEBUTTONUP       0x402
#define SDL_MOUSEWHEEL          0x403
#define SDL_WINDOWEVENT         0x200
#define SDL_WINDOWEVENT_FOCUS_GAINED  12
#define SDL_WINDOWEVENT_FOCUS_LOST    13
#define SDL_WINDOWEVENT_MAXIMIZED     10
#define SDL_WINDOWEVENT_MINIMIZED     11
#define SDL_WINDOWEVENT_RESIZED       5
#define SDL_WINDOWEVENT_RESTORED      9

#define SDL_BUTTON_LEFT   1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT  3
#define SDL_BUTTON(X)    (1u << ((X) - 1))

typedef struct SDL_KeyboardEvent {
    uint32_t   type;
    uint32_t   timestamp;
    uint32_t   windowID;
    uint8_t    state;
    uint8_t    repeat;
    uint8_t    padding2;
    uint8_t    padding3;
    SDL_Keysym keysym;
} SDL_KeyboardEvent;

typedef struct SDL_WindowEvent {
    uint32_t  type;
    uint32_t  timestamp;
    uint32_t  windowID;
    uint8_t   event;
    uint8_t   padding[3];
    int32_t   data1;
    int32_t   data2;
} SDL_WindowEvent;

typedef struct SDL_MouseButtonEvent {
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    uint32_t which;
    uint8_t  button;
    uint8_t  state;
    uint8_t  clicks;
    uint8_t  padding;
    int32_t  x;
    int32_t  y;
} SDL_MouseButtonEvent;

typedef struct SDL_MouseWheelEvent {
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    uint32_t which;
    int32_t  x;
    int32_t  y;
    uint32_t direction;
} SDL_MouseWheelEvent;

typedef union SDL_Event {
    uint32_t              type;
    SDL_KeyboardEvent     key;
    SDL_WindowEvent       window;
    SDL_MouseButtonEvent  button;
    SDL_MouseWheelEvent   wheel;
    uint8_t               padding[64];
} SDL_Event;

typedef struct SDL_SysWMinfo {
    int dummy;
} SDL_SysWMinfo;

#define SDL_VERSION(x)   do { (void)(x); } while(0)

/* ---- Funktions-Prototypen (Implementation in sdl_glue.cpp) ---- */
int          SDL_Init(uint32_t flags);
void         SDL_Quit(void);
const char*  SDL_GetError(void);

uint32_t     SDL_GetTicks(void);
void         SDL_Delay(uint32_t ms);

SDL_mutex*   SDL_CreateMutex(void);
void         SDL_DestroyMutex(SDL_mutex* m);
int          SDL_LockMutex(SDL_mutex* m);
int          SDL_UnlockMutex(SDL_mutex* m);

SDL_Window*    SDL_CreateWindow(const char* title, int x, int y, int w, int h, uint32_t flags);
SDL_Renderer*  SDL_CreateRenderer(SDL_Window* w, int idx, uint32_t flags);
SDL_Texture*   SDL_CreateTexture(SDL_Renderer* r, uint32_t fmt, int access, int w, int h);
int            SDL_SetWindowMinimumSize(SDL_Window* w, int min_w, int min_h);
int            SDL_SetWindowSize(SDL_Window* w, int width, int height);
int            SDL_SetWindowFullscreen(SDL_Window* w, uint32_t flags);
uint32_t       SDL_GetWindowID(SDL_Window* w);
void           SDL_GetWindowSize(SDL_Window* w, int* width, int* height);
int            SDL_GetWindowWMInfo(SDL_Window* w, SDL_SysWMinfo* info);
uint32_t       SDL_GetWindowPixelFormat(SDL_Window* w);
int            SDL_RenderSetLogicalSize(SDL_Renderer* r, int w, int h);
int            SDL_SetHint(const char* name, const char* value);
int            SDL_RenderClear(SDL_Renderer* r);
int            SDL_RenderCopy(SDL_Renderer* r, SDL_Texture* t,
                              const SDL_Rect* src, const SDL_Rect* dst);
void           SDL_RenderPresent(SDL_Renderer* r);
int            SDL_UpdateTexture(SDL_Texture* t, const SDL_Rect* rect,
                                 const void* pixels, int pitch);

/* Wolf4SDL übergibt `unsigned int*` (kein uint32_t*); auf ESP32-S3-newlib
 * ist uint32_t = `unsigned long`, also nicht typkompatibel. */
int            SDL_PixelFormatEnumToMasks(uint32_t fmt, int* bpp,
                                          unsigned int* rm, unsigned int* gm,
                                          unsigned int* bm, unsigned int* am);

SDL_Surface*  SDL_CreateRGBSurface(uint32_t flags, int w, int h, int depth,
                                   uint32_t rm, uint32_t gm, uint32_t bm, uint32_t am);
void          SDL_FreeSurface(SDL_Surface* s);
int           SDL_LockSurface(SDL_Surface* s);
void          SDL_UnlockSurface(SDL_Surface* s);
int           SDL_BlitSurface(SDL_Surface* src, const SDL_Rect* srcrect,
                              SDL_Surface* dst, SDL_Rect* dstrect);
int           SDL_FillRect(SDL_Surface* dst, const SDL_Rect* rect, uint32_t color);
int           SDL_SaveBMP(SDL_Surface* s, const char* path);

uint32_t      SDL_MapRGB(SDL_PixelFormat* fmt, uint8_t r, uint8_t g, uint8_t b);

SDL_Palette*  SDL_AllocPalette(int ncolors);
void          SDL_FreePalette(SDL_Palette* p);
int           SDL_SetPaletteColors(SDL_Palette* p, const SDL_Color* colors,
                                   int firstcolor, int ncolors);
int           SDL_SetSurfacePalette(SDL_Surface* s, SDL_Palette* p);

int           SDL_BuildAudioCVT(SDL_AudioCVT* cvt,
                                uint16_t src_fmt, uint8_t src_ch, int src_rate,
                                uint16_t dst_fmt, uint8_t dst_ch, int dst_rate);
int           SDL_ConvertAudio(SDL_AudioCVT* cvt);

int           SDL_PollEvent(SDL_Event* event);
int           SDL_WaitEvent(SDL_Event* event);
int           SDL_PushEvent(SDL_Event* event);

SDL_Keymod    SDL_GetModState(void);
uint32_t      SDL_GetRelativeMouseState(int* x, int* y);
int           SDL_SetRelativeMouseMode(int enabled);
void          SDL_WarpMouseInWindow(SDL_Window* w, int x, int y);

int             SDL_NumJoysticks(void);
SDL_Joystick*   SDL_JoystickOpen(int idx);
void            SDL_JoystickClose(SDL_Joystick* j);
int             SDL_JoystickEventState(int state);
int             SDL_JoystickNumButtons(SDL_Joystick* j);
int             SDL_JoystickNumHats(SDL_Joystick* j);
int             SDL_JoystickGetButton(SDL_Joystick* j, int button);
int16_t         SDL_JoystickGetAxis(SDL_Joystick* j, int axis);
uint8_t         SDL_JoystickGetHat(SDL_Joystick* j, int hat);
void            SDL_JoystickUpdate(void);

#ifdef __cplusplus
}
#endif

/* ---- POSIX-IO Redirect ----
 * Wolf4SDL nutzt open/read/write/lseek/close/stat/unlink/mkdir direkt.
 * Auf dem ESP32 hängt SD an Furi-Storage, nicht an newlib-VFS. Diese
 * Macros leiten alle Aufrufe in den Wolf4SDL-Sourcen auf wolf_*-Wrapper
 * um (Implementation in wolf3d_storage.cpp).
 *
 * SDL.h wird in wl_def.h *nach* <fcntl.h>/<sys/stat.h> included, daher
 * werden die System-Deklarationen nicht durch das Macro umbenannt — nur
 * die Aufrufstellen in den Wolf-Files. */

#ifdef __cplusplus
extern "C" {
#endif
int     wolf_open(const char* path, int flags, ...);
int     wolf_close(int fd);
int32_t wolf_read(int fd, void* buf, size_t n);
int32_t wolf_write(int fd, const void* buf, size_t n);
int32_t wolf_lseek(int fd, int32_t offset, int whence);
int     wolf_stat(const char* path, void* statbuf);
int     wolf_unlink(const char* path);
int     wolf_mkdir(const char* path, int mode);

/* stdio FILE* wrapper — Wolf4SDL nutzt fopen/fread/fseek/ftell/fwrite/fclose. */
typedef struct WolfFile WolfFile;
WolfFile* wolf_fopen(const char* path, const char* mode);
int       wolf_fclose(WolfFile* f);
size_t    wolf_fread(void* buf, size_t sz, size_t n, WolfFile* f);
size_t    wolf_fwrite(const void* buf, size_t sz, size_t n, WolfFile* f);
int       wolf_fseek(WolfFile* f, long off, int whence);
long      wolf_ftell(WolfFile* f);
int       wolf_feof(WolfFile* f);
#ifdef __cplusplus
}
#endif

/* Function-like macros: greifen nur bei `stat(p,sb)`-Aufrufen, nicht bei
 * `struct stat statbuf` (da kein `(`). Das vermeidet Konflikte mit der
 * struct-Definition aus <sys/stat.h>. */
#define open(...)        wolf_open(__VA_ARGS__)
#define close(fd)        wolf_close(fd)
#define read(fd, b, n)   wolf_read((fd), (b), (n))
#define write(fd, b, n)  wolf_write((fd), (b), (n))
#define lseek(fd, o, w)  wolf_lseek((fd), (o), (w))
#define stat(p, sb)      wolf_stat((p), (sb))
#define unlink(p)        wolf_unlink(p)
#define mkdir(p, m)      wolf_mkdir((p), (m))
#define _mkdir(p)        wolf_mkdir((p), 0755)

/* stdio-Redirect-Macros stehen am Ende von wl_def.h (nach <stdio.h>),
 * weil sie sonst in den Newlib-stdio.h-Deklarationen kollidieren würden. */
/* getenv → wolf_getenv: wir definieren ein Inline-Wrapper statt Macro,
 * weil ein function-like macro mit stdlib.hs `char *getenv(const char*)`
 * Deklaration kollidiert. */

#endif /* WOLF3D_SDL_SHIM_H */
