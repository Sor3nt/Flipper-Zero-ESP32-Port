/* Flipper port of doomgeneric — glue between the generic Doom core and the
 * Furi OS display / input / timer services. */

#include "doomgeneric.h"
#include "doomkeys.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_display.h>
#include <furi_hal_spi_bus.h>
#include <input/input.h>

#include <esp_lcd_panel_ops.h>
#include <esp_heap_caps.h>

#include <string.h>

#define DOOM_TAG "Doom"

#define KEYQ_SIZE 32

typedef struct {
    uint16_t data[KEYQ_SIZE]; /* high byte = pressed flag, low byte = doom key */
    volatile uint8_t wr;
    volatile uint8_t rd;
} DoomKeyQueue;

static DoomKeyQueue s_keyq;
static FuriMutex* s_keyq_mutex = NULL;
static uint16_t* s_rgb_stripe = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static uint16_t s_panel_w = 0;
static uint16_t s_panel_h = 0;

/* Called by the Flipper app before doomgeneric_Create() is invoked. */
void doom_flipper_runtime_init(esp_lcd_panel_handle_t panel, uint16_t w, uint16_t h) {
    s_panel = panel;
    s_panel_w = w;
    s_panel_h = h;
    if(!s_keyq_mutex) s_keyq_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
}

/* Push a (pressed, doom_key) pair into the queue. Safe to call from the
 * Furi input-service task. */
void doom_flipper_push_key(int pressed, unsigned char doom_key) {
    if(!s_keyq_mutex) return;
    /* Wait for the mutex — the queue must not drop events under load,
     * otherwise held keys stay "pressed" in Doom forever. */
    if(furi_mutex_acquire(s_keyq_mutex, 50) != FuriStatusOk) return;
    uint8_t next = (s_keyq.wr + 1) % KEYQ_SIZE;
    if(next != s_keyq.rd) {
        s_keyq.data[s_keyq.wr] = (uint16_t)((pressed ? 1 : 0) << 8) | (uint16_t)doom_key;
        s_keyq.wr = next;
    }
    furi_mutex_release(s_keyq_mutex);
}

/* ---- DG_* callbacks ---- */

/* Implemented in the doomgeneric library so we do not have to expose
 * private types (visplane_t, drawseg_t, ticcmd_set_t) across the .fam lib
 * boundary. They allocate ~137 KB from PSRAM which would otherwise have
 * sat in DRAM as static BSS and broken the BT/FreeRTOS queues at boot. */
extern void R_AllocPlaneBuffers(void);
extern void R_AllocDrawsegs(void);
extern void R_AllocViewAngleTox(void);
extern void R_AllocDrawTables(void);
extern void R_AllocVissprites(void);
extern void D_AllocTicdata(void);
extern void P_AllocIntercepts(void);
extern void R_AllocLightTables(void);
extern void I_AllocCapturedStats(void);
extern void I_InitMutableInfoTables(void);
extern void I_InitMutableSoundTables(void);

void DG_Init(void) {
    FURI_LOG_I(DOOM_TAG, "DG_Init (panel=%p res=%ux%u)", s_panel, s_panel_w, s_panel_h);
    I_InitMutableInfoTables();   /* states, mobjinfo */
    I_InitMutableSoundTables();  /* S_sfx, S_music */
    R_AllocPlaneBuffers();       /* visplanes, openings */
    R_AllocDrawsegs();
    R_AllocViewAngleTox();
    R_AllocDrawTables();         /* ylookup, columnofs */
    R_AllocVissprites();
    R_AllocLightTables();        /* scalelight, scalelightfixed, zlight */
    D_AllocTicdata();
    P_AllocIntercepts();
    I_AllocCapturedStats();
}

/* Convert Doom's 320x200 RGBA8888 buffer to RGB565 and blit to the 320x170
 * panel with vertical nearest-neighbour scaling (200 → 170). Doom's native
 * pixel aspect is 1.2:1 (non-square), so the 0.85x vertical compression
 * actually brings the image closer to a correct 1:1 pixel aspect. No
 * cropping, the full play area + HUD is visible. */
void DG_DrawFrame(void) {
    if(!s_panel || !s_rgb_stripe) return;

    const uint16_t panel_w = s_panel_w;
    const uint16_t panel_h = s_panel_h;
    const uint16_t doom_w = DOOMGENERIC_RESX;
    const uint16_t doom_h = DOOMGENERIC_RESY;

    const uint32_t* src = (const uint32_t*)DG_ScreenBuffer;
    const size_t stripe_h = 16;

    furi_hal_spi_bus_lock();
    for(uint16_t y0 = 0; y0 < panel_h; y0 += stripe_h) {
        uint16_t rows = (y0 + stripe_h > panel_h) ? (panel_h - y0) : stripe_h;
        for(uint16_t row = 0; row < rows; row++) {
            uint16_t panel_y = y0 + row;
            /* Nearest-neighbour vertical scale: map panel_y in [0..panel_h) to
             * doom_y in [0..doom_h). +panel_h/2 biases to the nearest row. */
            uint16_t doom_y = (uint16_t)(((uint32_t)panel_y * doom_h + panel_h / 2) / panel_h);
            if(doom_y >= doom_h) doom_y = doom_h - 1;
            const uint32_t* src_row = &src[(uint32_t)doom_y * doom_w];
            uint16_t* dst_row = &s_rgb_stripe[row * panel_w];
            uint16_t copy_w = doom_w < panel_w ? doom_w : panel_w;
            for(uint16_t x = 0; x < copy_w; x++) {
                uint32_t rgba = src_row[x];
                uint8_t r = (uint8_t)(rgba >> 16);
                uint8_t g = (uint8_t)(rgba >> 8);
                uint8_t b = (uint8_t)(rgba);
                uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
                dst_row[x] = (uint16_t)((c >> 8) | (c << 8));
            }
            /* Right-side letterbox fill if panel is wider than Doom */
            for(uint16_t x = copy_w; x < panel_w; x++) dst_row[x] = 0;
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, y0, panel_w, y0 + rows, s_rgb_stripe);
    }
    furi_hal_spi_bus_unlock();
}

void DG_SleepMs(uint32_t ms) {
    furi_delay_ms(ms);
}

uint32_t DG_GetTicksMs(void) {
    return (uint32_t)furi_get_tick() * (1000u / configTICK_RATE_HZ);
}

int DG_GetKey(int* pressed, unsigned char* doom_key) {
    if(!s_keyq_mutex) return 0;
    int got = 0;
    if(furi_mutex_acquire(s_keyq_mutex, 0) == FuriStatusOk) {
        if(s_keyq.rd != s_keyq.wr) {
            uint16_t pkt = s_keyq.data[s_keyq.rd];
            s_keyq.rd = (s_keyq.rd + 1) % KEYQ_SIZE;
            *pressed = (pkt >> 8) & 0xFF;
            *doom_key = pkt & 0xFF;
            got = 1;
        }
        furi_mutex_release(s_keyq_mutex);
    }
    return got;
}

void DG_SetWindowTitle(const char* title) {
    FURI_LOG_I(DOOM_TAG, "title: %s", title ? title : "(null)");
}

/* Runtime setup called from the Flipper app after doomgeneric_Create()
 * has allocated DG_ScreenBuffer. Allocates the stripe buffer in DMA-capable
 * internal RAM. */
void doom_flipper_alloc_stripe(void) {
    if(s_rgb_stripe) return;
    size_t bytes = (size_t)s_panel_w * 16 * sizeof(uint16_t);
    s_rgb_stripe = heap_caps_malloc(bytes, MALLOC_CAP_DMA);
    if(!s_rgb_stripe) {
        FURI_LOG_E(DOOM_TAG, "stripe alloc failed (%u bytes)", (unsigned)bytes);
    }
}

void doom_flipper_free_stripe(void) {
    if(s_rgb_stripe) {
        free(s_rgb_stripe);
        s_rgb_stripe = NULL;
    }
}
