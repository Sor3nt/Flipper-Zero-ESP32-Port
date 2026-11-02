#include "subghz_spectrum.h"
#include "../helpers/subghz_spectrum_worker.h"
#include "../helpers/subghz_click_echo.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_display.h>
#include <furi_hal_power.h>
#include <furi_hal_spi_bus.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification.h>
#include <string.h>

#include <esp_lcd_panel_ops.h>
#include <esp_heap_caps.h>

#define TAG "SubGhzSpectrum"

/* ---- Color display takeover (HackRF-style) --------------------------------
 * The Furi GUI renders a 128x64 mono canvas scaled to the ST7789. To draw in
 * color we bypass it: gui_direct_draw_acquire() pauses GUI commits, then we
 * blit an RGB565 frame straight to the panel (same pattern as the Doom port).
 * The frame is composed in a PSRAM buffer and pushed in DMA-capable DRAM
 * stripes (DMA cannot read PSRAM). RGB565 words are byte-swapped for the
 * ST7789 SPI byte order. */

#define WF_ROWS_MAX 96
/* Internal RAM that must be free to start: 2 FuriThread TCBs (~0.5 KB each),
 * mutexes and small queue objects, with slack. Deliberately small so Spectrum
 * is only refused when the firmware could not have created its threads anyway. */
#define SPECTRUM_INTERNAL_HEADROOM 2048
#define BINS SUBGHZ_SPECTRUM_BINS

/* RSSI window mapped to the display's dynamic range (dBm). */
#define RSSI_FLOOR (-95.0f)
#define RSSI_CEIL  (-30.0f)

#define CENTER_MIN 300000000UL
#define CENTER_MAX 928000000UL
#define CENTER_DEFAULT 433920000UL

/* Up/Down jump straight between the common ISM / remote frequencies. With only
 * span/16 steps on Left/Right, getting from 433.92 to 315 MHz took ~160 clicks,
 * so a remote on another band never showed up on screen. */
static const uint32_t subghz_spectrum_bands[] = {
    315000000UL,
    390000000UL,
    418000000UL,
    433920000UL,
    868350000UL,
    915000000UL,
};

/* The max-hold peak is reported once it is this far above the display floor. */
#define PEAK_REPORT_MARGIN_DB 6.0f

#define NBANDS COUNT_OF(subghz_spectrum_bands)
/* Auto-scan: a band whose strongest bin clears this margin over the display
 * floor counts as "active". The scan parks on an active band so the user can
 * see it, then resumes hopping after this many consecutive quiet sweeps. */
#define AUTO_DETECT_MARGIN_DB 10.0f
#define AUTO_QUIET_SWEEPS     8
/* How long (ms) a band stays flagged "recently active" in the band bar after
 * its last detection, so a brief burst stays visible instead of flickering. */
#define BAND_ACTIVE_HOLD_MS 2500

static const uint32_t subghz_spectrum_spans[] = {
    1000000UL,
    2000000UL,
    4000000UL,
    6000000UL,
    10000000UL,
    16000000UL,
};

typedef enum {
    SubGhzSpectrumAdjustCursor,
    SubGhzSpectrumAdjustCenter,
    SubGhzSpectrumAdjustSpan,
    SubGhzSpectrumAdjustAuto, // hands the frequency to the auto band scanner
    SubGhzSpectrumAdjustMAX,
} SubGhzSpectrumAdjust;

typedef struct {
    bool startup_failed;
} SubGhzSpectrumViewModel;

struct SubGhzSpectrum {
    View* view; // blank placeholder view for the ViewDispatcher
    SubGhzSpectrumCallback callback;
    void* context;

    SubGhzSpectrumWorker* worker;
    NotificationApp* notification;
    Gui* gui;
    FuriPubSub* input;
    FuriPubSubSubscription* input_sub;
    FuriThread* render_thread;
    FuriMutex* mutex;

    esp_lcd_panel_handle_t panel;
    uint16_t w;
    uint16_t h;
    uint16_t* fb; // PSRAM, w*h RGB565 (byte-swapped)
    uint8_t* wf; // PSRAM, WF_ROWS_MAX*BINS normalized power (0..255)
    uint16_t magma_lut[256]; // magma() colormap precomputed per 0..255 input (byte-swapped RGB565)

    volatile bool running;
    volatile bool dirty;
    bool light_sleep_inhibited;
    bool audio_tone_active;
    uint32_t start_center; // set by the scene before start()
    SubGhzClickEcho click_echo; // input pubsub thread only

    // model (mutex-protected)
    float rssi[BINS];
    float peak[BINS];
    uint32_t center;
    uint32_t span;
    uint16_t cursor;
    SubGhzSpectrumAdjust adjust;
    bool has_data;
    bool radio_fail; // the last sweep could not measure a single bin (not "no signal")

    /* Auto band scanner (adjust == SubGhzSpectrumAdjustAuto). Worker-thread
     * writes via the sweep callback; render + input read under the mutex. */
    int cur_band; // index into subghz_spectrum_bands of the window being swept
    bool auto_parked; // sitting on an active band instead of hopping
    uint32_t auto_quiet; // consecutive quiet sweeps while parked
    float band_peak[NBANDS]; // last strongest RSSI seen per band (dBm)
    uint32_t band_seen_ms[NBANDS]; // tick of last detection per band (0 = never)
};

/* Nearest band index for a center frequency (exact match in auto mode). */
static int subghz_spectrum_band_index(uint32_t center) {
    int best = 0;
    uint32_t best_d = UINT32_MAX;
    for(size_t i = 0; i < NBANDS; i++) {
        uint32_t d = center > subghz_spectrum_bands[i] ? center - subghz_spectrum_bands[i] :
                                                         subghz_spectrum_bands[i] - center;
        if(d < best_d) {
            best_d = d;
            best = (int)i;
        }
    }
    return best;
}

static void subghz_spectrum_set_startup_failed(SubGhzSpectrum* instance, bool failed) {
    with_view_model(
        instance->view,
        SubGhzSpectrumViewModel * model,
        { model->startup_failed = failed; },
        true);
}

/* ---- 5x7 font (column-major, bit0 = top row) ---------------------------- */
typedef struct {
    char c;
    uint8_t col[5];
} SubGhzGlyph;

static const SubGhzGlyph subghz_font[] = {
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}}, {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}}, {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}}, {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}}, {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}}, {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}}, {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'=', {0x14, 0x14, 0x14, 0x14, 0x14}}, {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}}, {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}}, {'z', {0x61, 0x51, 0x49, 0x45, 0x43}},
    {'d', {0x38, 0x44, 0x44, 0x48, 0x7F}}, {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'f', {0x08, 0x7E, 0x09, 0x01, 0x02}}, {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}}, {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}}, {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}}, {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'k', {0x7F, 0x10, 0x28, 0x44, 0x00}}, {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}}, {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
};

static const uint8_t* subghz_glyph(char c) {
    for(size_t i = 0; i < COUNT_OF(subghz_font); i++) {
        if(subghz_font[i].c == c) return subghz_font[i].col;
    }
    return NULL;
}

/* ---- color helpers ------------------------------------------------------ */
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8)); // byte-swap for ST7789
}

#define COL_BLACK  0x0000
#define COL_GRID   rgb565(38, 38, 44)
#define COL_TRACE  rgb565(255, 48, 40)
#define COL_HOLD   rgb565(0, 210, 210)
#define COL_CURSOR rgb565(255, 220, 0)
#define COL_ERROR  rgb565(255, 40, 40)
#define COL_TEXT   rgb565(220, 220, 220)
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

static float subghz_norm(float rssi) {
    float n = (rssi - RSSI_FLOOR) / (RSSI_CEIL - RSSI_FLOOR);
    if(n < 0.0f) n = 0.0f;
    if(n > 1.0f) n = 1.0f;
    return n;
}

/* ---- framebuffer primitives (operate on instance->fb, w*h) -------------- */
static inline void fb_px(SubGhzSpectrum* s, int x, int y, uint16_t c) {
    if(x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    s->fb[y * s->w + x] = c;
}

static void fb_vline(SubGhzSpectrum* s, int x, int y0, int y1, uint16_t c) {
    if(y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    for(int y = y0; y <= y1; y++) fb_px(s, x, y, c);
}

static void fb_hline(SubGhzSpectrum* s, int x0, int x1, int y, uint16_t c) {
    if(x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    for(int x = x0; x <= x1; x++) fb_px(s, x, y, c);
}

static void fb_line(SubGhzSpectrum* s, int x0, int y0, int x1, int y1, uint16_t c) {
    int dx = x1 - x0;
    if(dx < 0) dx = -dx;
    int dy = y1 - y0;
    if(dy < 0) dy = -dy;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for(;;) {
        fb_px(s, x0, y0, c);
        if(x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if(e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if(e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void fb_fillrect(SubGhzSpectrum* s, int x, int y, int w, int h, uint16_t c) {
    for(int j = 0; j < h; j++)
        for(int i = 0; i < w; i++) fb_px(s, x + i, y + j, c);
}

static int fb_char(SubGhzSpectrum* s, int x, int y, char ch, uint16_t c, int scale) {
    const uint8_t* g = subghz_glyph(ch);
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

static void fb_text(SubGhzSpectrum* s, int x, int y, const char* str, uint16_t c, int scale) {
    while(*str) {
        x = fb_char(s, x, y, *str, c, scale);
        str++;
    }
}

static int text_width(const char* str, int scale) {
    return (int)strlen(str) * 6 * scale;
}

static void fmt_mhz(uint32_t hz, char* buf, size_t n, int dec) {
    unsigned long mhz = (unsigned long)(hz / 1000000UL);
    if(dec <= 0) {
        snprintf(buf, n, "%lu", mhz);
    } else if(dec == 1) {
        snprintf(buf, n, "%lu.%01lu", mhz, (unsigned long)((hz % 1000000UL) / 100000UL));
    } else {
        snprintf(buf, n, "%lu.%02lu", mhz, (unsigned long)((hz % 1000000UL) / 10000UL));
    }
}

/* Band overview strip (auto mode): the configured bands laid out left to right
 * as equal segments, each labelled with its MHz. The band currently being swept
 * is outlined; a band seen active within BAND_ACTIVE_HOLD_MS is filled cyan so
 * the user can tell at a glance which frequency is transmitting. Occupies the
 * horizontal band from y0 to y1. */
static void subghz_spectrum_draw_band_bar(SubGhzSpectrum* s, int y0, int y1) {
    const int w = s->w;
    const uint32_t now = furi_get_tick();
    const uint32_t hold_ticks = furi_ms_to_ticks(BAND_ACTIVE_HOLD_MS);
    for(size_t i = 0; i < NBANDS; i++) {
        const int xa = (int)(i * (size_t)w / NBANDS);
        const int xb = (int)((i + 1) * (size_t)w / NBANDS);
        fb_vline(s, xb - 1, y0, y1, COL_GRID);

        const bool recent = s->band_seen_ms[i] != 0 && (now - s->band_seen_ms[i]) < hold_ticks;
        const uint16_t label_col = recent ? COL_HOLD : COL_TEXT_DIM;

        char lbl[8];
        fmt_mhz(subghz_spectrum_bands[i], lbl, sizeof(lbl), 0); // integer MHz
        const int lw = text_width(lbl, 1);
        fb_text(s, xa + (xb - xa - lw) / 2, y0 + 1, lbl, label_col, 1);

        // Activity underline for a recently active band.
        if(recent) fb_fillrect(s, xa + 2, y1 - 3, (xb - xa) - 4, 2, COL_HOLD);

        // Outline the band currently being swept.
        if((int)i == s->cur_band) {
            fb_hline(s, xa, xb - 1, y0, COL_CURSOR);
            fb_hline(s, xa, xb - 1, y1 - 1, COL_CURSOR);
            fb_vline(s, xa, y0, y1 - 1, COL_CURSOR);
            fb_vline(s, xb - 1, y0, y1 - 1, COL_CURSOR);
        }
    }
}

/* ---- frame composition (holds the data mutex) --------------------------- */
static void subghz_spectrum_compose(SubGhzSpectrum* s) {
    const int w = s->w;
    const int h = s->h;
    const int spec_h = (h * 56) / 100; // spectrum region height (~95)
    const bool auto_mode = (s->adjust == SubGhzSpectrumAdjustAuto);
    // Auto mode reserves an extra strip under the readout for the band bar.
    const int plot_top = auto_mode ? 30 : 16; // reserve top strip for the readout
    const int plot_bottom = spec_h - 10; // reserve bottom strip for freq ticks
    const int plot_h = plot_bottom - plot_top;
    const int wf_h = h - spec_h;

    memset(s->fb, 0, (size_t)w * h * sizeof(uint16_t)); // black

    // --- grid ---
    for(int i = 0; i <= 8; i++) {
        int x = i * (w - 1) / 8;
        fb_vline(s, x, plot_top, plot_bottom, COL_GRID);
    }
    for(int i = 0; i <= 4; i++) {
        int y = plot_top + i * plot_h / 4;
        fb_hline(s, 0, w - 1, y, COL_GRID);
    }

    uint32_t f_start = s->center - s->span / 2;
    uint32_t f_end = s->center + s->span / 2;
    uint32_t stepf = s->span / BINS;
    uint32_t cursor_freq = f_start + (uint32_t)s->cursor * stepf + stepf / 2;

    // --- traces (max-hold cyan, then live red) ---
    if(s->has_data) {
        int px = 0, py_hold = 0, py = 0;
        for(int i = 0; i < BINS; i++) {
            int x = i * (w - 1) / (BINS - 1);
            int yh = plot_bottom - (int)(subghz_norm(s->peak[i]) * plot_h);
            int yl = plot_bottom - (int)(subghz_norm(s->rssi[i]) * plot_h);
            if(i > 0) {
                fb_line(s, px, py_hold, x, yh, COL_HOLD);
                fb_line(s, px, py, x, yl, COL_TRACE);
            }
            px = x;
            py_hold = yh;
            py = yl;
        }
    }

    // --- cursor (hidden in auto mode: Left/Right don't move it there) ---
    int cx = s->cursor * (w - 1) / (BINS - 1);
    if(!auto_mode) fb_vline(s, cx, plot_top, plot_bottom, COL_CURSOR);

    // --- waterfall ---
    for(int r = 0; r < wf_h && r < WF_ROWS_MAX; r++) {
        uint8_t* row = &s->wf[r * BINS];
        int y = spec_h + r;
        for(int x = 0; x < w; x++) {
            int bin = x * BINS / w;
            if(bin >= BINS) bin = BINS - 1;
            fb_px(s, x, y, s->magma_lut[row[bin]]);
        }
    }
    // cursor column marker in the waterfall
    if(!auto_mode) fb_vline(s, cx, spec_h, h - 1, COL_CURSOR);

    // --- text: readout ---
    char buf[40];
    const char* tag = (s->adjust == SubGhzSpectrumAdjustCursor) ? "CUR" :
                      (s->adjust == SubGhzSpectrumAdjustCenter) ? "CEN" :
                      (s->adjust == SubGhzSpectrumAdjustSpan)   ? "SPN" :
                                                                  "AUTO";
    fb_text(s, 2, 2, tag, COL_CURSOR, 2);
    if(auto_mode) {
        // Band overview between the readout line and the plot.
        subghz_spectrum_draw_band_bar(s, 17, plot_top - 1);
    }
    /* Secondary readout starts past the widest tag ("AUTO" = 4 chars * 12 px),
     * so it never overlaps the mode tag. */
    const int readout_x = 2 + 4 * 12 + 4;
    if(s->radio_fail) {
        // Distinguish a dead radio from an empty band; both look flat otherwise.
        fb_text(s, readout_x, 2, "NO RX", COL_ERROR, 2);
    } else if(s->has_data) {
        // Strongest max-hold bin, so a signal is found without walking the cursor.
        int pk = 0;
        for(int i = 1; i < BINS; i++) {
            if(s->peak[i] > s->peak[pk]) pk = i;
        }
        if(s->peak[pk] > RSSI_FLOOR + PEAK_REPORT_MARGIN_DB) {
            char pbuf[16];
            char line[32];
            fmt_mhz(f_start + (uint32_t)pk * stepf + stepf / 2, pbuf, sizeof(pbuf), 2);
            snprintf(line, sizeof(line), "PK %s %ddB", pbuf, (int)s->peak[pk]);
            fb_text(s, readout_x, 4, line, COL_HOLD, 1);
        }
    }

    char fbuf[16];
    if(auto_mode) {
        // No cursor in auto mode; show the band currently being swept.
        fmt_mhz(s->center, fbuf, sizeof(fbuf), 2);
        snprintf(buf, sizeof(buf), "f=%s", fbuf);
    } else {
        fmt_mhz(cursor_freq, fbuf, sizeof(fbuf), 2);
        if(s->has_data) {
            snprintf(buf, sizeof(buf), "f=%s P=%ddB", fbuf, (int)s->rssi[s->cursor]);
        } else {
            snprintf(buf, sizeof(buf), "f=%s", fbuf);
        }
    }
    fb_text(s, w - text_width(buf, 1) - 2, 4, buf, COL_TEXT, 1);

    // --- text: frequency ticks along the spectrum baseline ---
    fmt_mhz(f_start, buf, sizeof(buf), 1);
    fb_text(s, 1, spec_h - 8, buf, COL_TEXT_DIM, 1);
    fmt_mhz(s->center, buf, sizeof(buf), 2);
    fb_text(s, w / 2 - text_width(buf, 1) / 2, spec_h - 8, buf, COL_TEXT_DIM, 1);
    fmt_mhz(f_end, buf, sizeof(buf), 1);
    fb_text(s, w - text_width(buf, 1) - 1, spec_h - 8, buf, COL_TEXT_DIM, 1);
}

/* ---- push composed frame to the panel in DMA stripes -------------------- */
static void subghz_spectrum_blit(SubGhzSpectrum* s) {
    // The HAL bounces the PSRAM frame through its own boot-time DMA stripe, so
    // the Spectrum needs no internal DMA RAM of its own (that allocation was the
    // one that failed with "Not enough memory" once qFlipper/USB had taken it).
    // It waits for each stripe's DMA and locks the shared bus per stripe, so
    // SD/radio traffic can interleave.
    if(!furi_hal_display_blit_buffered(0, 0, s->w, s->h, s->fb)) {
        FURI_LOG_E(TAG, "Display blit failed (%ux%u)", s->w, s->h);
    }
}

static int32_t subghz_spectrum_render_thread(void* context) {
    SubGhzSpectrum* s = context;

    s->gui = furi_record_open(RECORD_GUI);
    gui_direct_draw_acquire(s->gui);
    s->panel = furi_hal_display_get_panel_handle();
    if(!s->panel) FURI_LOG_E(TAG, "No LCD panel handle: nothing can be drawn");
    uint32_t frames = 0;

    while(s->running) {
        /* Screen off (backlight timeout, panel in SLPIN): nothing is visible, so
         * do not compose ~150 KB and DMA it over the bus the CC1101 sweeps on.
         * That traffic took the shared SPI lock per stripe and slowed the sweep
         * while the user could not even see it. Keep the frame marked dirty so
         * the first frame after the panel wakes is a full, current one. */
        if(furi_hal_display_is_asleep()) {
            s->dirty = true;
            furi_delay_ms(50);
            continue;
        }
        if(s->dirty && s->panel && s->fb && s->wf) {
            furi_mutex_acquire(s->mutex, FuriWaitForever);
            s->dirty = false;
            subghz_spectrum_compose(s);
            furi_mutex_release(s->mutex);
            subghz_spectrum_blit(s);
            if(frames++ == 0) FURI_LOG_I(TAG, "First frame drawn (%ux%u)", s->w, s->h);
        }
        /* Poll faster than the ~9 Hz sweep rate so a finished sweep and any input
         * (cursor, band change) reach the panel within one short tick instead of
         * up to 25 ms later. A redraw still only happens when something changed
         * (the dirty flag), so this costs a bool check, not extra blits. */
        furi_delay_ms(12);
    }

    gui_direct_draw_release(s->gui);
    furi_record_close(RECORD_GUI);
    s->gui = NULL;
    return 0;
}

/* ---- input (runs on the input pubsub thread) ---------------------------- */
static void subghz_spectrum_reset_data(SubGhzSpectrum* s) {
    for(int i = 0; i < BINS; i++) {
        s->rssi[i] = RSSI_FLOOR;
        s->peak[i] = RSSI_FLOOR;
    }
    memset(s->wf, 0, (size_t)WF_ROWS_MAX * BINS);
    s->has_data = false;
    s->radio_fail = false;
}

static size_t subghz_span_index(uint32_t span) {
    for(size_t i = 0; i < COUNT_OF(subghz_spectrum_spans); i++) {
        if(subghz_spectrum_spans[i] == span) return i;
    }
    return 3;
}

static void subghz_spectrum_input_callback(const void* value, void* context) {
    SubGhzSpectrum* s = context;
    const InputEvent* event = value;
    if(!s->running) return;

    // Back is handled by subghz_spectrum_view_input() instead: the GUI keeps
    // forwarding keys to the placeholder view, and leaving from here raced
    // that delivery - the rest of the Back sequence then reached the previous
    // scene and navigated back a second time.
    if(event->key == InputKeyBack) return;

    // OK short -> cycle the active control (CUR -> CEN -> SPN -> AUTO)
    if(event->key == InputKeyOk && event->type == InputTypeShort) {
        bool enter_auto = false;
        uint32_t auto_center = 0, auto_span = 0;
        furi_mutex_acquire(s->mutex, FuriWaitForever);
        s->adjust = (s->adjust + 1) % SubGhzSpectrumAdjustMAX;
        if(s->adjust == SubGhzSpectrumAdjustAuto) {
            // Start the hop from the nearest band to where the user was looking.
            s->cur_band = subghz_spectrum_band_index(s->center);
            s->center = subghz_spectrum_bands[s->cur_band];
            s->auto_parked = false;
            s->auto_quiet = 0;
            subghz_spectrum_reset_data(s);
            enter_auto = true;
            auto_center = s->center;
            auto_span = s->span;
        }
        s->dirty = true;
        furi_mutex_release(s->mutex);
        if(enter_auto) subghz_spectrum_worker_set_window(s->worker, auto_center, auto_span);
        return;
    }

    bool is_dir = (event->key == InputKeyUp) || (event->key == InputKeyDown) ||
                  (event->key == InputKeyLeft) || (event->key == InputKeyRight);
    if(!is_dir) return;
    // Keep the legacy-board click filter here; Paradoxt now emits one physical
    // Press/Release pair and does not need its synthetic-echo path.
    if(subghz_click_echo_filter(&s->click_echo, event)) return;
    bool move = (event->type == InputTypePress) || (event->type == InputTypeRepeat);
    if(!move) return;

    int dir = (event->key == InputKeyDown || event->key == InputKeyRight) ? 1 : -1;
    bool changed_window = false;
    uint32_t new_center = 0, new_span = 0;

    furi_mutex_acquire(s->mutex, FuriWaitForever);
    if(event->key == InputKeyUp || event->key == InputKeyDown) {
        // Up/Down: next/previous band, whatever control is active.
        const bool up = event->key == InputKeyUp;
        uint32_t target = s->center;
        if(up) {
            for(size_t i = 0; i < COUNT_OF(subghz_spectrum_bands); i++) {
                if(subghz_spectrum_bands[i] > s->center) {
                    target = subghz_spectrum_bands[i];
                    break;
                }
            }
        } else {
            for(size_t i = COUNT_OF(subghz_spectrum_bands); i-- > 0;) {
                if(subghz_spectrum_bands[i] < s->center) {
                    target = subghz_spectrum_bands[i];
                    break;
                }
            }
        }
        if(target != s->center) {
            s->center = target;
            subghz_spectrum_reset_data(s);
            changed_window = true;
        }
        // In auto mode a manual band pick re-seeds the hop from here.
        s->cur_band = subghz_spectrum_band_index(s->center);
        s->auto_parked = false;
        s->auto_quiet = 0;
        new_center = s->center;
        new_span = s->span;
        s->dirty = true;
        furi_mutex_release(s->mutex);
        if(changed_window) subghz_spectrum_worker_set_window(s->worker, new_center, new_span);
        return;
    }
    switch(s->adjust) {
    case SubGhzSpectrumAdjustCursor: {
        int c = (int)s->cursor + dir;
        if(c < 0) c = 0;
        if(c >= BINS) c = BINS - 1;
        s->cursor = (uint16_t)c;
        break;
    }
    case SubGhzSpectrumAdjustCenter: {
        uint32_t stepc = s->span / 16;
        if(stepc < 50000) stepc = 50000;
        int64_t nc = (int64_t)s->center + (int64_t)dir * stepc;
        if(nc < (int64_t)CENTER_MIN) nc = CENTER_MIN;
        if(nc > (int64_t)CENTER_MAX) nc = CENTER_MAX;
        s->center = (uint32_t)nc;
        subghz_spectrum_reset_data(s);
        changed_window = true;
        break;
    }
    case SubGhzSpectrumAdjustSpan: {
        size_t idx = subghz_span_index(s->span);
        if(dir > 0 && idx + 1 < COUNT_OF(subghz_spectrum_spans))
            idx++;
        else if(dir < 0 && idx > 0)
            idx--;
        s->span = subghz_spectrum_spans[idx];
        subghz_spectrum_reset_data(s);
        changed_window = true;
        break;
    }
    default:
        break;
    }
    new_center = s->center;
    new_span = s->span;
    s->dirty = true;
    furi_mutex_release(s->mutex);

    if(changed_window) subghz_spectrum_worker_set_window(s->worker, new_center, new_span);
}

/* ---- worker sweep callback (runs on the worker thread) ------------------ */
static void subghz_spectrum_sweep_callback(
    void* context,
    const float* rssi,
    uint32_t center,
    uint32_t span) {
    SubGhzSpectrum* s = context;
    size_t strongest_bin = 0;
    float strongest_rssi = RSSI_FLOOR;
    bool any_measured = false;
    for(size_t i = 0; i < BINS; i++) {
        // The worker marks bins it could not measure (SPI error, radio not in RX,
        // outside the CC1101 bands) with exactly -127 dBm.
        if(rssi[i] != -127.0f) any_measured = true;
        if(rssi[i] > strongest_rssi) {
            strongest_rssi = rssi[i];
            strongest_bin = i;
        }
    }

    furi_mutex_acquire(s->mutex, FuriWaitForever);
    if(s->wf && s->center == center && s->span == span && !any_measured) {
        // Do not scroll an all-floor row into the waterfall: it would look like a
        // quiet band while the radio is actually dead. A window centered outside
        // the CC1101 bands is empty by design and is not flagged.
        if(furi_hal_subghz_is_frequency_valid(center) && !s->radio_fail) {
            s->radio_fail = true;
            s->dirty = true;
        }
    } else if(s->wf && s->center == center && s->span == span) {
        s->radio_fail = false;
        // scroll waterfall down (row 0 = newest)
        memmove(&s->wf[BINS], &s->wf[0], (size_t)(WF_ROWS_MAX - 1) * BINS);
        for(int i = 0; i < BINS; i++) {
            s->rssi[i] = rssi[i];
            s->peak[i] -= 0.5f;
            if(rssi[i] > s->peak[i]) s->peak[i] = rssi[i];
            s->wf[i] = (uint8_t)(subghz_norm(rssi[i]) * 255.0f);
        }
        s->has_data = true;
        s->dirty = true;
    }

    /* Auto band scanner: record this band's activity, park on an active band so
     * the user can read it, and otherwise hop to the next band. Decided under
     * the mutex; the worker window change is requested after releasing it. */
    bool auto_hop = false;
    uint32_t auto_center = 0, auto_span = span;
    if(s->adjust == SubGhzSpectrumAdjustAuto && s->center == center && s->span == span) {
        const int b = subghz_spectrum_band_index(center);
        const bool active =
            any_measured && strongest_rssi > (RSSI_FLOOR + AUTO_DETECT_MARGIN_DB);
        s->cur_band = b;
        s->band_peak[b] = any_measured ? strongest_rssi : RSSI_FLOOR;
        if(active) {
            s->band_seen_ms[b] = furi_get_tick();
            s->auto_parked = true;
            s->auto_quiet = 0;
        } else if(s->auto_parked) {
            if(++s->auto_quiet >= AUTO_QUIET_SWEEPS) {
                s->auto_parked = false;
                s->auto_quiet = 0;
            }
        }
        if(!s->auto_parked) {
            const int nb = (b + 1) % (int)NBANDS;
            s->cur_band = nb;
            s->center = subghz_spectrum_bands[nb];
            subghz_spectrum_reset_data(s);
            auto_center = s->center;
            auto_hop = true;
        }
        s->dirty = true;
    }
    furi_mutex_release(s->mutex);

    if(auto_hop) subghz_spectrum_worker_set_window(s->worker, auto_center, auto_span);

    if(strongest_rssi > RSSI_FLOOR + 8.0f) {
        const float strength = subghz_norm(strongest_rssi);
        const float tone_hz = 300.0f + (1700.0f * (float)strongest_bin / (float)(BINS - 1));
        const float volume = 0.15f + 0.55f * strength;
        notification_message_tone(s->notification, tone_hz, volume);
        s->audio_tone_active = true;
    } else if(s->audio_tone_active) {
        notification_message_tone(s->notification, 0.0f, 0.0f);
        s->audio_tone_active = false;
    }
}

/* ---- lifecycle ---------------------------------------------------------- */
void subghz_spectrum_set_callback(
    SubGhzSpectrum* instance,
    SubGhzSpectrumCallback callback,
    void* context) {
    furi_assert(instance);
    instance->callback = callback;
    instance->context = context;
}

bool subghz_spectrum_start(SubGhzSpectrum* instance) {
    furi_assert(instance);
    subghz_spectrum_set_startup_failed(instance, false);

    instance->w = furi_hal_display_get_h_res();
    instance->h = furi_hal_display_get_v_res();

    instance->fb =
        heap_caps_malloc((size_t)instance->w * instance->h * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    instance->wf = heap_caps_malloc((size_t)WF_ROWS_MAX * BINS, MALLOC_CAP_SPIRAM);

    /* No DMA-capable buffer of our own: furi_hal_display_blit_buffered() bounces
     * the PSRAM frame through the display HAL's stripe, allocated once at boot.
     * A private stripe from MALLOC_CAP_DMA is what made Spectrum fail with "Not
     * enough memory" whenever internal RAM was used up (qFlipper/USB composite,
     * Wi-Fi, BT), however far the stripe was shrunk. */
    if(!instance->fb || !instance->wf) {
        FURI_LOG_E(
            TAG,
            "Spectrum buffers unavailable: PSRAM free=%u largest=%u",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        heap_caps_free(instance->fb);
        heap_caps_free(instance->wf);
        instance->fb = NULL;
        instance->wf = NULL;
        subghz_spectrum_set_startup_failed(instance, true);
        return false;
    }

    /* Two thread control blocks and a few mutexes must live in internal RAM, and
     * their allocators abort the firmware when that fails (furi_thread_alloc has
     * no NULL path). Before the private stripe was dropped, the stripe allocation
     * above refused to start in this situation; keep refusing gracefully instead
     * of crashing: probe for the headroom first. */
    void* internal_probe =
        heap_caps_malloc(SPECTRUM_INTERNAL_HEADROOM, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!internal_probe) {
        FURI_LOG_E(
            TAG,
            "Not enough internal RAM for the sweep/render threads: free=%u largest=%u",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        heap_caps_free(instance->fb);
        heap_caps_free(instance->wf);
        instance->fb = NULL;
        instance->wf = NULL;
        subghz_spectrum_set_startup_failed(instance, true);
        return false;
    }
    heap_caps_free(internal_probe);

    // Precompute the magma colormap once: the waterfall samples it per pixel
    // from an 8-bit power value, so a 256-entry LUT replaces ~24k float ops per
    // frame with a table lookup (bit-identical output).
    for(int i = 0; i < 256; i++) {
        instance->magma_lut[i] = magma((float)i / 255.0f);
    }

    instance->center = (furi_hal_subghz_is_frequency_valid(instance->start_center) &&
                        instance->start_center >= CENTER_MIN &&
                        instance->start_center <= CENTER_MAX) ?
                           instance->start_center :
                           CENTER_DEFAULT;
    instance->span = 6000000UL;
    memset(&instance->click_echo, 0, sizeof(instance->click_echo));
    instance->cursor = BINS / 2;
    instance->adjust = SubGhzSpectrumAdjustCursor;
    subghz_spectrum_reset_data(instance);
    instance->dirty = true;

    // Worker
    instance->worker = subghz_spectrum_worker_alloc();
    if(!instance->worker) {
        FURI_LOG_E(TAG, "Unable to allocate spectrum worker");
        heap_caps_free(instance->fb);
        heap_caps_free(instance->wf);
        instance->fb = NULL;
        instance->wf = NULL;
        subghz_spectrum_set_startup_failed(instance, true);
        return false;
    }
    subghz_spectrum_worker_set_window(instance->worker, instance->center, instance->span);
    subghz_spectrum_worker_set_callback(
        instance->worker, subghz_spectrum_sweep_callback, instance);
    instance->notification = furi_record_open(RECORD_NOTIFICATION);

    // Input
    instance->input = furi_record_open(RECORD_INPUT_EVENTS);
    instance->input_sub =
        furi_pubsub_subscribe(instance->input, subghz_spectrum_input_callback, instance);

    // Render thread (takes over the display)
    instance->running = true;
    instance->render_thread = furi_thread_alloc_ex(
        "SubGhzSpectrumRender", 4096, subghz_spectrum_render_thread, instance);
    if(!instance->render_thread) {
        FURI_LOG_E(TAG, "Unable to allocate spectrum render thread");
        instance->running = false;
        furi_pubsub_unsubscribe(instance->input, instance->input_sub);
        instance->input_sub = NULL;
        furi_record_close(RECORD_INPUT_EVENTS);
        instance->input = NULL;
        furi_record_close(RECORD_NOTIFICATION);
        instance->notification = NULL;
        subghz_spectrum_worker_free(instance->worker);
        instance->worker = NULL;
        heap_caps_free(instance->fb);
        heap_caps_free(instance->wf);
        instance->fb = NULL;
        instance->wf = NULL;
        subghz_spectrum_set_startup_failed(instance, true);
        return false;
    }
    furi_thread_start(instance->render_thread);

    furi_hal_power_light_sleep_inhibit_enter();
    instance->light_sleep_inhibited = true;
    subghz_spectrum_worker_start(instance->worker);
    FURI_LOG_I(
        TAG,
        "Started: center=%lu Hz span=%lu Hz panel=%s internal free=%u largest=%u",
        (unsigned long)instance->center,
        (unsigned long)instance->span,
        furi_hal_display_get_panel_handle() ? "ok" : "MISSING",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return true;
}

void subghz_spectrum_set_start_frequency(SubGhzSpectrum* instance, uint32_t frequency) {
    furi_assert(instance);
    instance->start_center = frequency;
}

void subghz_spectrum_stop(SubGhzSpectrum* instance) {
    furi_assert(instance);
    if(!instance->render_thread) return; // start() was never called

    // Stop sweeps first (frees the SPI bus for the final render teardown).
    if(instance->worker && subghz_spectrum_worker_is_running(instance->worker)) {
        subghz_spectrum_worker_stop(instance->worker);
    }
    if(instance->audio_tone_active && instance->notification) {
        notification_message_tone(instance->notification, 0.0f, 0.0f);
        instance->audio_tone_active = false;
    }
    if(instance->light_sleep_inhibited) {
        furi_hal_power_light_sleep_inhibit_exit();
        instance->light_sleep_inhibited = false;
    }

    // Stop the render loop; the thread releases direct-draw before it returns.
    instance->running = false;
    furi_thread_join(instance->render_thread);
    furi_thread_free(instance->render_thread);
    instance->render_thread = NULL;

    if(instance->input_sub) {
        furi_pubsub_unsubscribe(instance->input, instance->input_sub);
        instance->input_sub = NULL;
    }
    if(instance->input) {
        furi_record_close(RECORD_INPUT_EVENTS);
        instance->input = NULL;
    }

    if(instance->worker) {
        subghz_spectrum_worker_free(instance->worker);
        instance->worker = NULL;
    }
    if(instance->notification) {
        furi_record_close(RECORD_NOTIFICATION);
        instance->notification = NULL;
    }

    if(instance->fb) {
        heap_caps_free(instance->fb);
        instance->fb = NULL;
    }
    if(instance->wf) {
        heap_caps_free(instance->wf);
        instance->wf = NULL;
    }
}

/* Blank placeholder view — the color frame is drawn via direct display
 * takeover, so this view only exists to keep the ViewDispatcher happy. */
static void subghz_spectrum_view_draw(Canvas* canvas, void* model) {
    const SubGhzSpectrumViewModel* view_model = model;
    if(!canvas || !view_model || !view_model->startup_failed) return;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas,
        64,
        26,
        AlignCenter,
        AlignCenter,
        "Spectrum unavailable");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignCenter, "Not enough memory");
}

/* Runs on the ViewDispatcher thread, so the exit event is queued strictly
 * after this key event: the remaining events of the Back sequence still find
 * this view as their target and cannot trigger a second navigation. */
static bool subghz_spectrum_view_input(InputEvent* event, void* context) {
    SubGhzSpectrum* instance = context;
    if(!instance->running) {
        // Startup failed (or already leaving): let the dispatcher navigate.
        return false;
    }
    if(event->key == InputKeyBack &&
       (event->type == InputTypeShort || event->type == InputTypeLong)) {
        instance->running = false;
        if(instance->callback) {
            instance->callback(SubGhzCustomEventViewSpectrumBack, instance->context);
        }
    }
    // Every other key is handled by the input pubsub subscription.
    return true;
}

SubGhzSpectrum* subghz_spectrum_alloc(void) {
    SubGhzSpectrum* instance = malloc(sizeof(SubGhzSpectrum));
    furi_check(instance);
    memset(instance, 0, sizeof(SubGhzSpectrum));
    instance->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_check(instance->mutex);
    instance->view = view_alloc();
    furi_check(instance->view);
    view_allocate_model(
        instance->view, ViewModelTypeLocking, sizeof(SubGhzSpectrumViewModel));
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, subghz_spectrum_view_draw);
    view_set_input_callback(instance->view, subghz_spectrum_view_input);
    subghz_spectrum_set_startup_failed(instance, false);
    return instance;
}

void subghz_spectrum_free(SubGhzSpectrum* instance) {
    furi_assert(instance);
    subghz_spectrum_stop(instance);
    view_free(instance->view);
    furi_mutex_free(instance->mutex);
    free(instance);
}

View* subghz_spectrum_get_view(SubGhzSpectrum* instance) {
    furi_assert(instance);
    return instance->view;
}
