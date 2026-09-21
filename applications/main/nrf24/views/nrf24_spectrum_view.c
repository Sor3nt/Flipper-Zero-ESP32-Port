#include "nrf24_spectrum_view.h"
#include "../nrf24_hw.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_display.h>
#include <furi_hal_spi_bus.h>
#include <gui/gui.h>
#include <input/input.h>
#include <string.h>
#include <stdio.h>

#include <esp_lcd_panel_ops.h>
#include <esp_heap_caps.h>

#define TAG "Nrf24Spectrum"

/* ---- Color display takeover (same approach as subghz_spectrum.c) --------
 * The Furi GUI renders a 128x64 mono canvas scaled to the ST7789. To draw a
 * proper grayscale/color waterfall we bypass it: gui_direct_draw_acquire()
 * pauses GUI commits, then we blit an RGB565 frame straight to the panel at
 * its full native resolution. The frame is composed in a PSRAM buffer and
 * pushed in DMA-capable DRAM stripes (DMA cannot read PSRAM). */

#define WF_ROWS_MAX 96
#define STRIPE_H    17

#define BINS NRF24_SPECTRUM_CHANNELS

struct Nrf24Spectrum {
    View* view; // blank placeholder view for the ViewDispatcher

    FuriThread* sweep_thread;
    FuriThread* render_thread;
    FuriMutex* mutex;
    Gui* gui;

    esp_lcd_panel_handle_t panel;
    uint16_t w;
    uint16_t h;
    uint16_t* fb; // PSRAM, w*h RGB565 (byte-swapped)
    uint16_t* stripe; // DRAM DMA, w*STRIPE_H
    uint8_t* wf; // PSRAM, WF_ROWS_MAX*BINS normalized level (0..255)

    volatile bool sweep_running;
    volatile bool render_running;
    volatile bool dirty;

    // model (mutex-protected)
    uint8_t levels[BINS];
    uint32_t sweep_count;
    bool hardware_ok;
};

/* ---- 5x7 font (column-major, bit0 = top row) — the handful of glyphs
 * this view's readout actually needs. ---------------------------------- */
typedef struct {
    char c;
    uint8_t col[5];
} Nrf24Glyph;

static const Nrf24Glyph nrf24_font[] = {
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}}, {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}}, {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}}, {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}}, {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}}, {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}}, {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}}, {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}}, {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
};

static const uint8_t* nrf24_glyph(char c) {
    for(size_t i = 0; i < COUNT_OF(nrf24_font); i++) {
        if(nrf24_font[i].c == c) return nrf24_font[i].col;
    }
    return NULL;
}

/* ---- color helpers ------------------------------------------------------ */
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8)); // byte-swap for ST7789
}

#define COL_GRID     rgb565(38, 38, 44)
#define COL_TRACE    rgb565(255, 48, 40)
#define COL_TITLE    rgb565(255, 220, 0)
#define COL_TEXT     rgb565(220, 220, 220)
#define COL_TEXT_DIM rgb565(150, 150, 150)

/* magma colormap, 5 control points */
static const uint8_t magma_ctrl[5][3] = {
    {0, 0, 4},
    {80, 18, 123},
    {182, 54, 121},
    {251, 136, 97},
    {252, 253, 191},
};

static uint16_t magma(float t) {
    if(t < 0.0f) t = 0.0f;
    if(t > 1.0f) t = 1.0f;
    float f = t * 4.0f;
    int i = (int)f;
    if(i > 3) i = 3;
    float frac = f - i;
    uint8_t r = (uint8_t)(magma_ctrl[i][0] + (magma_ctrl[i + 1][0] - magma_ctrl[i][0]) * frac);
    uint8_t g = (uint8_t)(magma_ctrl[i][1] + (magma_ctrl[i + 1][1] - magma_ctrl[i][1]) * frac);
    uint8_t b = (uint8_t)(magma_ctrl[i][2] + (magma_ctrl[i + 1][2] - magma_ctrl[i][2]) * frac);
    return rgb565(r, g, b);
}

/* 1.5x boost so weak/noisy channels are still visible; clamp to [0,1].
 * Matches the boost the old mono bargraph applied. */
static float nrf24_norm(uint8_t level) {
    float n = (level * 3.0f) / (125.0f * 2.0f);
    if(n < 0.0f) n = 0.0f;
    if(n > 1.0f) n = 1.0f;
    return n;
}

/* ---- framebuffer primitives (operate on instance->fb, w*h) -------------- */
static inline void fb_px(Nrf24Spectrum* s, int x, int y, uint16_t c) {
    if(x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    s->fb[y * s->w + x] = c;
}

static void fb_vline(Nrf24Spectrum* s, int x, int y0, int y1, uint16_t c) {
    if(y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    for(int y = y0; y <= y1; y++) fb_px(s, x, y, c);
}

static void fb_hline(Nrf24Spectrum* s, int x0, int x1, int y, uint16_t c) {
    if(x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    for(int x = x0; x <= x1; x++) fb_px(s, x, y, c);
}

static void fb_fillrect(Nrf24Spectrum* s, int x, int y, int w, int h, uint16_t c) {
    for(int j = 0; j < h; j++)
        for(int i = 0; i < w; i++) fb_px(s, x + i, y + j, c);
}

static int fb_char(Nrf24Spectrum* s, int x, int y, char ch, uint16_t c, int scale) {
    const uint8_t* g = nrf24_glyph(ch);
    if(g) {
        for(int col = 0; col < 5; col++) {
            uint8_t bits = g[col];
            for(int row = 0; row < 7; row++) {
                if(bits & (1 << row)) {
                    fb_fillrect(s, x + col * scale, y + row * scale, scale, scale, c);
                }
            }
        }
    }
    return x + 6 * scale; // advance (5 + 1 gap)
}

static void fb_text(Nrf24Spectrum* s, int x, int y, const char* str, uint16_t c, int scale) {
    while(*str) {
        x = fb_char(s, x, y, *str, c, scale);
        str++;
    }
}

static int text_width(const char* str, int scale) {
    return (int)strlen(str) * 6 * scale;
}

/* ---- frame composition (holds the data mutex) --------------------------- */
static void nrf24_spectrum_compose(Nrf24Spectrum* s) {
    const int w = s->w;
    const int h = s->h;
    memset(s->fb, 0, (size_t)w * h * sizeof(uint16_t)); // black

    if(!s->hardware_ok) {
        fb_text(s, w / 2 - text_width("NRF24 ERR", 2) / 2, h / 2 - 7, "NRF24 ERR", COL_TRACE, 2);
        return;
    }

    const int spec_h = (h * 56) / 100; // spectrum region height
    const int plot_top = 16; // reserve top strip for the title/readout
    const int plot_bottom = spec_h - 10; // reserve bottom strip for channel ticks
    const int plot_h = plot_bottom - plot_top;
    const int wf_h = h - spec_h;

    // --- grid ---
    for(int i = 0; i <= 8; i++) {
        int x = i * (w - 1) / 8;
        fb_vline(s, x, plot_top, plot_bottom, COL_GRID);
    }
    for(int i = 0; i <= 4; i++) {
        int y = plot_top + i * plot_h / 4;
        fb_hline(s, 0, w - 1, y, COL_GRID);
    }

    // --- live bargraph ---
    for(int ch = 0; ch < BINS; ch++) {
        int x0 = ch * w / BINS;
        int x1 = (ch + 1) * w / BINS;
        int bw = x1 - x0 - 1;
        if(bw < 1) bw = 1;
        int bh = (int)(nrf24_norm(s->levels[ch]) * plot_h);
        if(bh > 0) fb_fillrect(s, x0, plot_bottom - bh, bw, bh, COL_TRACE);
    }
    fb_hline(s, 0, w - 1, plot_bottom, COL_GRID);

    // --- waterfall (row 0 = newest, scrolls down) ---
    for(int r = 0; r < wf_h && r < WF_ROWS_MAX; r++) {
        uint8_t* row = &s->wf[r * BINS];
        int y = spec_h + r;
        for(int x = 0; x < w; x++) {
            int bin = x * BINS / w;
            if(bin >= BINS) bin = BINS - 1;
            fb_px(s, x, y, magma(row[bin] / 255.0f));
        }
    }

    // --- text: title + sweep counter ---
    fb_text(s, 2, 2, "NRF24", COL_TITLE, 2);
    char buf[24];
    snprintf(buf, sizeof(buf), "S:%lu", (unsigned long)s->sweep_count);
    fb_text(s, w - text_width(buf, 1) - 2, 4, buf, COL_TEXT, 1);

    // --- text: channel ticks along the spectrum baseline ---
    char tick[4];
    for(int ch = 0; ch <= 70; ch += 10) {
        int x0 = ch * w / BINS;
        int x1 = (ch + 1) * w / BINS;
        int x = x0 + (x1 - x0) / 2;
        snprintf(tick, sizeof(tick), "%d", ch);
        fb_text(s, x - text_width(tick, 1) / 2, spec_h - 8, tick, COL_TEXT_DIM, 1);
    }
}

/* ---- push composed frame to the panel in DMA stripes -------------------- */
static void nrf24_spectrum_blit(Nrf24Spectrum* s) {
    const int w = s->w;
    const int h = s->h;
    furi_hal_spi_bus_lock();
    for(int y0 = 0; y0 < h; y0 += STRIPE_H) {
        int rows = (y0 + STRIPE_H > h) ? (h - y0) : STRIPE_H;
        memcpy(s->stripe, &s->fb[(size_t)y0 * w], (size_t)rows * w * sizeof(uint16_t));
        esp_lcd_panel_draw_bitmap(s->panel, 0, y0, w, y0 + rows, s->stripe);
    }
    furi_hal_spi_bus_unlock();
}

static int32_t nrf24_spectrum_render_thread(void* context) {
    Nrf24Spectrum* s = context;

    s->gui = furi_record_open(RECORD_GUI);
    gui_direct_draw_acquire(s->gui);
    s->panel = furi_hal_display_get_panel_handle();

    while(s->render_running) {
        if(s->dirty && s->panel && s->fb && s->stripe && s->wf) {
            furi_mutex_acquire(s->mutex, FuriWaitForever);
            s->dirty = false;
            nrf24_spectrum_compose(s);
            furi_mutex_release(s->mutex);
            nrf24_spectrum_blit(s);
        }
        furi_delay_ms(25);
    }

    gui_direct_draw_release(s->gui);
    furi_record_close(RECORD_GUI);
    s->gui = NULL;
    return 0;
}

/* ---- channel sweep (runs on its own thread; owns the NRF24 SPI access) -- */
static int32_t nrf24_spectrum_sweep_thread(void* context) {
    Nrf24Spectrum* s = context;

    nrf24_hw_init();
    nrf24_hw_acquire();
    bool ok = nrf24_hw_probe();
    nrf24_hw_release();

    furi_mutex_acquire(s->mutex, FuriWaitForever);
    s->hardware_ok = ok;
    s->dirty = true;
    furi_mutex_release(s->mutex);

    if(!ok) {
        FURI_LOG_W(TAG, "NRF24 probe failed");
        nrf24_hw_deinit();
        return 0;
    }

    uint8_t local_levels[BINS] = {0};

    while(s->sweep_running) {
        nrf24_hw_acquire();
        for(uint8_t ch = 0; ch < BINS && s->sweep_running; ch++) {
            uint8_t rpd = nrf24_hw_listen_rpd(ch);
            local_levels[ch] = (uint8_t)((local_levels[ch] * 3 + rpd * 125) / 4);
        }
        nrf24_hw_release();

        furi_mutex_acquire(s->mutex, FuriWaitForever);
        memcpy(s->levels, local_levels, BINS);
        // scroll waterfall down (row 0 = newest)
        memmove(&s->wf[BINS], &s->wf[0], (size_t)(WF_ROWS_MAX - 1) * BINS);
        for(int i = 0; i < BINS; i++) {
            s->wf[i] = (uint8_t)(nrf24_norm(local_levels[i]) * 255.0f);
        }
        s->sweep_count++;
        s->dirty = true;
        furi_mutex_release(s->mutex);

        furi_delay_ms(20);
    }

    nrf24_hw_acquire();
    nrf24_hw_power_down();
    nrf24_hw_release();
    nrf24_hw_deinit();

    return 0;
}

/* ---- input ---------------------------------------------------------------
 * No custom controls — Back is handled by the view dispatcher navigation,
 * same as the other nrf24 views (see nrf24_scan_view.c). */
static bool nrf24_spectrum_input_callback(InputEvent* event, void* context) {
    UNUSED(event);
    UNUSED(context);
    return false;
}

/* Blank placeholder — the color frame is drawn via direct display takeover,
 * so this view only exists to keep the ViewDispatcher happy. */
static void nrf24_spectrum_view_draw(Canvas* canvas, void* model) {
    UNUSED(model);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "NRF24 Spectrum");
}

/* ---- lifecycle ------------------------------------------------------------ */
void nrf24_spectrum_start(Nrf24Spectrum* instance) {
    furi_assert(instance);

    instance->w = furi_hal_display_get_h_res();
    instance->h = furi_hal_display_get_v_res();

    instance->fb =
        heap_caps_malloc((size_t)instance->w * instance->h * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    instance->wf = heap_caps_malloc((size_t)WF_ROWS_MAX * BINS, MALLOC_CAP_SPIRAM);
    instance->stripe =
        heap_caps_malloc((size_t)instance->w * STRIPE_H * sizeof(uint16_t), MALLOC_CAP_DMA);

    memset(instance->levels, 0, sizeof(instance->levels));
    memset(instance->wf, 0, (size_t)WF_ROWS_MAX * BINS);
    instance->sweep_count = 0;
    instance->hardware_ok = true; // optimistic until the probe says otherwise
    instance->dirty = true;

    instance->sweep_running = true;
    instance->sweep_thread = furi_thread_alloc_ex(
        "Nrf24SpectrumSweep", 4096, nrf24_spectrum_sweep_thread, instance);
    furi_thread_start(instance->sweep_thread);

    // Render thread takes over the display.
    instance->render_running = true;
    instance->render_thread = furi_thread_alloc_ex(
        "Nrf24SpectrumRender", 4096, nrf24_spectrum_render_thread, instance);
    furi_thread_start(instance->render_thread);
}

void nrf24_spectrum_stop(Nrf24Spectrum* instance) {
    furi_assert(instance);
    if(!instance->sweep_thread && !instance->render_thread) return; // start() never called

    // Stop the NRF24 sweep first (frees the shared SPI bus for the final
    // render/blit before the render thread releases the display).
    if(instance->sweep_thread) {
        instance->sweep_running = false;
        furi_thread_join(instance->sweep_thread);
        furi_thread_free(instance->sweep_thread);
        instance->sweep_thread = NULL;
    }

    if(instance->render_thread) {
        instance->render_running = false;
        furi_thread_join(instance->render_thread);
        furi_thread_free(instance->render_thread);
        instance->render_thread = NULL;
    }

    if(instance->fb) {
        free(instance->fb);
        instance->fb = NULL;
    }
    if(instance->wf) {
        free(instance->wf);
        instance->wf = NULL;
    }
    if(instance->stripe) {
        free(instance->stripe);
        instance->stripe = NULL;
    }
}

Nrf24Spectrum* nrf24_spectrum_alloc(void) {
    Nrf24Spectrum* instance = malloc(sizeof(Nrf24Spectrum));
    memset(instance, 0, sizeof(Nrf24Spectrum));
    instance->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    instance->view = view_alloc();
    view_set_draw_callback(instance->view, nrf24_spectrum_view_draw);
    view_set_input_callback(instance->view, nrf24_spectrum_input_callback);
    return instance;
}

void nrf24_spectrum_free(Nrf24Spectrum* instance) {
    furi_assert(instance);
    nrf24_spectrum_stop(instance);
    view_free(instance->view);
    furi_mutex_free(instance->mutex);
    free(instance);
}

View* nrf24_spectrum_get_view(Nrf24Spectrum* instance) {
    furi_assert(instance);
    return instance->view;
}
