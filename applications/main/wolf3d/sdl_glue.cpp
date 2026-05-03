/* sdl_glue.cpp — Implementation der SDL-Mini-Shim für Wolf3D auf ESP32.
 *
 * Software-only Backend:
 *   - Surfaces: 8-bit indexed (Wolf3Ds Native), oder höher als opake Buffer.
 *   - BlitSurface / FillRect: Software-Loops auf 8-bit-Bytes.
 *   - Palette / Color: speichern, MapRGB liefert nearest-match-Index.
 *   - RenderPresent: greift auf das globale `screenBuffer` zu (Wolf4SDL-Symbol)
 *     und blittet 8-bit + Palette → RGB565 → Panel via wolf3d_glue.
 *   - Audio / Joystick / Mouse: Stubs.
 *   - Events: aus PubSub gespeiste Queue (siehe wolf3d_input.cpp).
 */

#include "SDL.h"
#include "wolf3d_glue.h"

#include <string.h>
#include <stdlib.h>

#include <furi.h>
#include <esp_heap_caps.h>

#define TAG "Wolf3D-SDL"

/* Wolf4SDL globals — verlinkt erst im Lib-Build. Wir greifen lazy zu, deshalb
 * weak-imports: wenn das Symbol fehlt (frühe Init), null. */
extern "C" SDL_Surface*   screenBuffer __attribute__((weak));
extern "C" unsigned       bufferPitch  __attribute__((weak));

/* Palette-Mirror für Debug-Logs (wolf3d_glue.cpp hat den Master-Buffer). */
static uint8_t s_palette_dbg[256 * 3] = {0};

/* Aus wolf3d_input.cpp */
extern "C" int  wolf3d_event_pop(SDL_Event* event);

/* ---- Init / Teardown ---- */

extern "C" int SDL_Init(uint32_t flags) {
    (void)flags;
    return 0;
}

extern "C" void SDL_Quit(void) {}

extern "C" const char* SDL_GetError(void) {
    return "";
}

/* ---- Time ---- */

extern "C" uint32_t SDL_GetTicks(void) {
    return wolf3d_glue_ticks_ms();
}

extern "C" void SDL_Delay(uint32_t ms) {
    wolf3d_glue_delay_ms(ms);
}

/* ---- Mutex (Furi-backed) ---- */

struct SDL_mutex {
    FuriMutex* m;
};

extern "C" SDL_mutex* SDL_CreateMutex(void) {
    SDL_mutex* mx = (SDL_mutex*)heap_caps_malloc(sizeof(SDL_mutex), MALLOC_CAP_INTERNAL);
    if(!mx) return nullptr;
    mx->m = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!mx->m) { heap_caps_free(mx); return nullptr; }
    return mx;
}

extern "C" void SDL_DestroyMutex(SDL_mutex* m) {
    if(!m) return;
    if(m->m) furi_mutex_free(m->m);
    heap_caps_free(m);
}

extern "C" int SDL_LockMutex(SDL_mutex* m) {
    if(!m || !m->m) return -1;
    return furi_mutex_acquire(m->m, FuriWaitForever) == FuriStatusOk ? 0 : -1;
}

extern "C" int SDL_UnlockMutex(SDL_mutex* m) {
    if(!m || !m->m) return -1;
    return furi_mutex_release(m->m) == FuriStatusOk ? 0 : -1;
}

/* ---- Window / Renderer / Texture (Stubs) ----
 * Forward-deklariert in SDL.h — wir geben Magic-Pointer zurück, die nie
 * dereferenziert werden. */

static char s_magic_window[1];
static char s_magic_renderer[1];
static char s_magic_texture[1];

extern "C" SDL_Window* SDL_CreateWindow(const char*, int, int, int, int, uint32_t) {
    return reinterpret_cast<SDL_Window*>(&s_magic_window);
}
extern "C" SDL_Renderer* SDL_CreateRenderer(SDL_Window*, int, uint32_t) {
    return reinterpret_cast<SDL_Renderer*>(&s_magic_renderer);
}
extern "C" SDL_Texture* SDL_CreateTexture(SDL_Renderer*, uint32_t, int, int, int) {
    return reinterpret_cast<SDL_Texture*>(&s_magic_texture);
}
extern "C" int SDL_SetWindowMinimumSize(SDL_Window*, int, int) { return 0; }
extern "C" int SDL_SetWindowSize(SDL_Window*, int, int) { return 0; }
extern "C" int SDL_SetWindowFullscreen(SDL_Window*, uint32_t) { return 0; }
extern "C" uint32_t SDL_GetWindowID(SDL_Window*) { return 1; }
extern "C" void SDL_GetWindowSize(SDL_Window*, int* w, int* h) { if(w) *w = 320; if(h) *h = 200; }
extern "C" int SDL_GetWindowWMInfo(SDL_Window*, SDL_SysWMinfo*) { return SDL_FALSE; }
extern "C" uint32_t SDL_GetWindowPixelFormat(SDL_Window*) {
    /* SDL_PIXELFORMAT_RGB888 — brauchen wir nur als plausibler Wert für
     * SDL_PixelFormatEnumToMasks. */
    return 0x16161804u;
}
extern "C" int SDL_RenderSetLogicalSize(SDL_Renderer*, int, int) { return 0; }
extern "C" int SDL_SetHint(const char*, const char*) { return SDL_TRUE; }
extern "C" int SDL_RenderClear(SDL_Renderer*) { return 0; }
extern "C" int SDL_RenderCopy(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_Rect*) { return 0; }
extern "C" int SDL_UpdateTexture(SDL_Texture*, const SDL_Rect*, const void*, int) {
    /* No-op: wir nehmen direkt den 8-bit-Buffer in RenderPresent. */
    return 0;
}

extern "C" void SDL_RenderPresent(SDL_Renderer*) {
    static uint32_t s_present_count = 0;
    s_present_count++;
    if(!&screenBuffer || !screenBuffer || !screenBuffer->pixels) {
        if(s_present_count <= 3) FURI_LOG_I(TAG, "RenderPresent: NO BUFFER");
        return;
    }
    if(s_present_count <= 3 || s_present_count % 60 == 0) {
        const uint8_t* p = (const uint8_t*)screenBuffer->pixels;
        /* Hex-dump erste 32 Pixel + Anzahl non-zero */
        size_t total = (size_t)screenBuffer->w * screenBuffer->h;
        size_t nz = 0;
        for(size_t i = 0; i < total; i++) if(p[i]) { nz++; if(nz > 100) break; }
        FURI_LOG_I(TAG,
            "RenderPresent #%lu nz>=%u/%u px[0..7]=%02x %02x %02x %02x %02x %02x %02x %02x  pal[0..3]={%02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x}",
            (unsigned long)s_present_count, (unsigned)nz, (unsigned)total,
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
            s_palette_dbg[0], s_palette_dbg[1], s_palette_dbg[2],
            s_palette_dbg[3], s_palette_dbg[4], s_palette_dbg[5],
            s_palette_dbg[6], s_palette_dbg[7], s_palette_dbg[8],
            s_palette_dbg[9], s_palette_dbg[10], s_palette_dbg[11]);
    }
    wolf3d_glue_present_indexed(
        static_cast<const uint8_t*>(screenBuffer->pixels),
        screenBuffer->w, screenBuffer->h, screenBuffer->pitch);
}

/* ---- Pixel-Format ---- */

extern "C" int SDL_PixelFormatEnumToMasks(uint32_t, int* bpp,
                                          unsigned int* rm, unsigned int* gm,
                                          unsigned int* bm, unsigned int* am) {
    if(bpp) *bpp = 32;
    if(rm)  *rm = 0x00FF0000;
    if(gm)  *gm = 0x0000FF00;
    if(bm)  *bm = 0x000000FF;
    if(am)  *am = 0x00000000;
    return SDL_TRUE;
}

/* ---- Surface ---- */

static SDL_PixelFormat* alloc_pixfmt(int depth) {
    SDL_PixelFormat* f = (SDL_PixelFormat*)heap_caps_calloc(1, sizeof(SDL_PixelFormat), MALLOC_CAP_SPIRAM);
    if(!f) return nullptr;
    f->BitsPerPixel = static_cast<uint8_t>(depth);
    f->BytesPerPixel = static_cast<uint8_t>((depth + 7) / 8);
    if(depth == 8) {
        f->palette = (SDL_Palette*)heap_caps_calloc(1, sizeof(SDL_Palette), MALLOC_CAP_SPIRAM);
        if(f->palette) {
            f->palette->ncolors = 256;
            f->palette->refcount = 1;
        }
    }
    return f;
}

static void free_pixfmt(SDL_PixelFormat* f) {
    if(!f) return;
    if(f->palette) {
        if(--f->palette->refcount <= 0) heap_caps_free(f->palette);
    }
    heap_caps_free(f);
}

extern "C" SDL_Surface* SDL_CreateRGBSurface(uint32_t, int w, int h, int depth,
                                             uint32_t rm, uint32_t gm,
                                             uint32_t bm, uint32_t am) {
    SDL_Surface* s = (SDL_Surface*)heap_caps_calloc(1, sizeof(SDL_Surface), MALLOC_CAP_SPIRAM);
    if(!s) return nullptr;
    s->format = alloc_pixfmt(depth);
    if(!s->format) { heap_caps_free(s); return nullptr; }
    s->format->Rmask = rm; s->format->Gmask = gm;
    s->format->Bmask = bm; s->format->Amask = am;
    s->w = w; s->h = h;
    int bpp = (depth + 7) / 8;
    s->pitch = w * bpp;
    /* 8-bit indexed Buffers können großzügig sein (320×200 = 64KB), in PSRAM.
     * RGB-Buffers nehmen wir auch PSRAM. */
    size_t bytes = static_cast<size_t>(s->pitch) * h;
    s->pixels = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if(!s->pixels) {
        FURI_LOG_E(TAG, "SDL_CreateRGBSurface: OOM (%dx%d depth=%d, %zu bytes)",
                   w, h, depth, bytes);
        free_pixfmt(s->format);
        heap_caps_free(s);
        return nullptr;
    }
    memset(s->pixels, 0, bytes);
    s->clip_rect.x = 0;
    s->clip_rect.y = 0;
    s->clip_rect.w = w;
    s->clip_rect.h = h;
    s->refcount = 1;
    return s;
}

extern "C" void SDL_FreeSurface(SDL_Surface* s) {
    if(!s) return;
    if(s->pixels) heap_caps_free(s->pixels);
    free_pixfmt(s->format);
    heap_caps_free(s);
}

extern "C" int SDL_LockSurface(SDL_Surface*) { return 0; }
extern "C" void SDL_UnlockSurface(SDL_Surface*) {}

/* Software 8-bit blit. ColorKey wird (noch) nicht unterstützt — Wolf3D
 * benutzt das hauptsächlich für Sprites, was in der vergangenen Pipeline
 * vor dem Blit auf screenBuffer aufgelöst wird. */
extern "C" int SDL_BlitSurface(SDL_Surface* src, const SDL_Rect* srcrect,
                               SDL_Surface* dst, SDL_Rect* dstrect) {
    if(!src || !dst || !src->pixels || !dst->pixels) return -1;
    int sx = srcrect ? srcrect->x : 0;
    int sy = srcrect ? srcrect->y : 0;
    int sw = srcrect ? srcrect->w : src->w;
    int sh = srcrect ? srcrect->h : src->h;
    int dx = dstrect ? dstrect->x : 0;
    int dy = dstrect ? dstrect->y : 0;

    /* Clip auf dst */
    if(dx < 0) { sx -= dx; sw += dx; dx = 0; }
    if(dy < 0) { sy -= dy; sh += dy; dy = 0; }
    if(dx + sw > dst->w) sw = dst->w - dx;
    if(dy + sh > dst->h) sh = dst->h - dy;
    if(sw <= 0 || sh <= 0) return 0;

    int bpp = src->format ? src->format->BytesPerPixel : 1;
    if(!bpp) bpp = 1;
    for(int row = 0; row < sh; row++) {
        const uint8_t* sp = (const uint8_t*)src->pixels + (sy + row) * src->pitch + sx * bpp;
        uint8_t* dp = (uint8_t*)dst->pixels + (dy + row) * dst->pitch + dx * bpp;
        memcpy(dp, sp, static_cast<size_t>(sw) * bpp);
    }
    if(dstrect) { dstrect->w = sw; dstrect->h = sh; }
    return 0;
}

extern "C" int SDL_FillRect(SDL_Surface* dst, const SDL_Rect* rect, uint32_t color) {
    if(!dst || !dst->pixels) return -1;
    int x = rect ? rect->x : 0;
    int y = rect ? rect->y : 0;
    int w = rect ? rect->w : dst->w;
    int h = rect ? rect->h : dst->h;
    if(x < 0) { w += x; x = 0; }
    if(y < 0) { h += y; y = 0; }
    if(x + w > dst->w) w = dst->w - x;
    if(y + h > dst->h) h = dst->h - y;
    if(w <= 0 || h <= 0) return 0;

    int bpp = dst->format ? dst->format->BytesPerPixel : 1;
    if(bpp == 1) {
        uint8_t c = static_cast<uint8_t>(color);
        for(int row = 0; row < h; row++) {
            uint8_t* p = (uint8_t*)dst->pixels + (y + row) * dst->pitch + x;
            memset(p, c, static_cast<size_t>(w));
        }
    } else if(bpp == 4) {
        for(int row = 0; row < h; row++) {
            uint32_t* p = (uint32_t*)((uint8_t*)dst->pixels + (y + row) * dst->pitch + x * 4);
            for(int col = 0; col < w; col++) p[col] = color;
        }
    }
    return 0;
}

extern "C" int SDL_SaveBMP(SDL_Surface*, const char*) {
    return -1; /* Screenshots brauchen wir nicht. */
}

/* ---- Palette ---- */

extern "C" SDL_Palette* SDL_AllocPalette(int ncolors) {
    /* Bewusst PSRAM — Wolf4SDL alloziert während des Spielverlaufs viele
     * Paletten (Fade-In/Out etc.), das würde sonst DRAM ausschlachten. */
    SDL_Palette* p = (SDL_Palette*)heap_caps_calloc(1, sizeof(SDL_Palette), MALLOC_CAP_SPIRAM);
    if(!p) return nullptr;
    p->ncolors = ncolors > 256 ? 256 : ncolors;
    p->refcount = 1;
    return p;
}

extern "C" void SDL_FreePalette(SDL_Palette* p) {
    if(!p) return;
    if(--p->refcount <= 0) heap_caps_free(p);
}

extern "C" int SDL_SetPaletteColors(SDL_Palette* p, const SDL_Color* colors,
                                    int firstcolor, int ncolors) {
    if(!p || !colors) return -1;
    if(firstcolor < 0 || firstcolor >= 256) return -1;
    int n = ncolors;
    if(firstcolor + n > 256) n = 256 - firstcolor;
    for(int i = 0; i < n; i++) p->colors[firstcolor + i] = colors[i];
    p->version++;

    /* Wenn das die "aktive" Palette ist (an screenBuffer gehängt),
     * liefern wir sie gleich an unsere Pipeline.
     * Plus: wir spiegeln die ersten 16 Einträge auch wenn nicht direkt
     * an screenBuffer gehängt — Wolf4SDL setzt Paletten oft auf die
     * "screen"-Surface (RGBA) oder einen separaten SDL_Palette und
     * macht den Apply-Step erst beim BlitSurface 8→32. Wir wollen die
     * Palette-Werte trotzdem für unsere Direkt-Pipeline. */
    uint8_t rgb[256 * 3];
    for(int i = 0; i < n; i++) {
        rgb[i * 3 + 0] = colors[i].r;
        rgb[i * 3 + 1] = colors[i].g;
        rgb[i * 3 + 2] = colors[i].b;
        if(firstcolor + i < 256) {
            s_palette_dbg[(firstcolor + i) * 3 + 0] = colors[i].r;
            s_palette_dbg[(firstcolor + i) * 3 + 1] = colors[i].g;
            s_palette_dbg[(firstcolor + i) * 3 + 2] = colors[i].b;
        }
    }
    wolf3d_glue_set_palette(rgb, firstcolor, n);
    static int s_setpal_count = 0;
    if(++s_setpal_count <= 4) {
        FURI_LOG_I(TAG, "SetPaletteColors first=%d n=%d (call#%d)",
                   firstcolor, n, s_setpal_count);
    }
    return 0;
}

extern "C" int SDL_SetSurfacePalette(SDL_Surface* s, SDL_Palette* p) {
    if(!s || !s->format) return -1;
    if(s->format->palette && --s->format->palette->refcount <= 0)
        heap_caps_free(s->format->palette);
    s->format->palette = p;
    if(p) p->refcount++;
    return 0;
}

extern "C" uint32_t SDL_MapRGB(SDL_PixelFormat* fmt, uint8_t r, uint8_t g, uint8_t b) {
    if(fmt && fmt->BitsPerPixel == 8 && fmt->palette) {
        /* Nearest-color search */
        int best = 0;
        int best_d = 1 << 30;
        for(int i = 0; i < fmt->palette->ncolors; i++) {
            int dr = fmt->palette->colors[i].r - r;
            int dg = fmt->palette->colors[i].g - g;
            int db = fmt->palette->colors[i].b - b;
            int d = dr * dr + dg * dg + db * db;
            if(d < best_d) { best_d = d; best = i; if(!d) break; }
        }
        return static_cast<uint32_t>(best);
    }
    /* RGB888 */
    return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

/* ---- Audio (Stubs) ---- */

extern "C" int SDL_BuildAudioCVT(SDL_AudioCVT* cvt, uint16_t, uint8_t, int,
                                 uint16_t, uint8_t, int) {
    if(cvt) { cvt->needed = 0; cvt->len_mult = 1; cvt->len_ratio = 1.0; }
    return 0;
}

extern "C" int SDL_ConvertAudio(SDL_AudioCVT* cvt) {
    if(cvt) cvt->len_cvt = cvt->len;
    return 0;
}

/* ---- Joystick (kein HW-Joystick) ---- */

extern "C" int SDL_NumJoysticks(void) { return 0; }
extern "C" SDL_Joystick* SDL_JoystickOpen(int) { return nullptr; }
extern "C" void SDL_JoystickClose(SDL_Joystick*) {}
extern "C" int SDL_JoystickEventState(int) { return 0; }
extern "C" int SDL_JoystickNumButtons(SDL_Joystick*) { return 0; }
extern "C" int SDL_JoystickNumHats(SDL_Joystick*) { return 0; }
extern "C" int SDL_JoystickGetButton(SDL_Joystick*, int) { return 0; }
extern "C" int16_t SDL_JoystickGetAxis(SDL_Joystick*, int) { return 0; }
extern "C" uint8_t SDL_JoystickGetHat(SDL_Joystick*, int) { return 0; }
extern "C" void SDL_JoystickUpdate(void) {}

/* ---- Mouse (Stubs) ---- */

extern "C" SDL_Keymod SDL_GetModState(void) { return 0; }
extern "C" uint32_t SDL_GetRelativeMouseState(int* x, int* y) { if(x) *x = 0; if(y) *y = 0; return 0; }
extern "C" int SDL_SetRelativeMouseMode(int) { return 0; }
extern "C" void SDL_WarpMouseInWindow(SDL_Window*, int, int) {}

/* ---- Events ---- */

extern "C" int SDL_PollEvent(SDL_Event* event) {
    return wolf3d_event_pop(event);
}

extern "C" int SDL_WaitEvent(SDL_Event* event) {
    while(!wolf3d_event_pop(event)) {
        wolf3d_glue_delay_ms(5);
    }
    return 1;
}

extern "C" int SDL_PushEvent(SDL_Event*) {
    return 1;
}
