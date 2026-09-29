#include "subghz_spectrum.h"
#include "../helpers/subghz_spectrum_worker.h"
#include "../helpers/subghz_spectrum_math.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_display.h>
#include <furi_hal_spi_bus.h>
#include <gui/gui.h>
#include <input/input.h>
#include <string.h>

#include <esp_lcd_panel_ops.h>
#include <esp_heap_caps.h>
#include <stdatomic.h>

#define TAG "SubGhzSpectrum"

/* ---- Color display takeover (HackRF-style) --------------------------------
 * The Furi GUI renders a 128x64 mono canvas scaled to the ST7789. To draw in
 * color we bypass it: gui_direct_draw_acquire() pauses GUI commits, then we
 * blit an RGB565 frame straight to the panel (same pattern as the Doom port).
 * The frame is composed in a PSRAM buffer and pushed in DMA-capable DRAM
 * stripes (DMA cannot read PSRAM). RGB565 words are byte-swapped for the
 * ST7789 SPI byte order. */

#define WF_ROWS_MAX 96
/* Match the existing LCD SPI bus maximum: 320 * 8 * 2 bytes. */
#define STRIPE_H    8

#define BINS SUBGHZ_SPECTRUM_BINS

/* RSSI window mapped to the display's dynamic range (dBm). */
#define RSSI_FLOOR (-95.0f)
#define RSSI_CEIL  (-30.0f)

#define CENTER_MIN 300000000UL
#define CENTER_MAX 928000000UL

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
    SubGhzSpectrumAdjustStep,
    SubGhzSpectrumAdjustDwell,
    SubGhzSpectrumAdjustThreshold,
    SubGhzSpectrumAdjustHold,
    SubGhzSpectrumAdjustRun,
    SubGhzSpectrumAdjustMAX,
} SubGhzSpectrumAdjust;

struct SubGhzSpectrum {
    View* view; // blank placeholder view for the ViewDispatcher
    SubGhzSpectrumCallback callback;
    void* context;

    SubGhzSpectrumWorker* worker;
    Gui* gui;
    FuriPubSub* input;
    FuriPubSubSubscription* input_sub;
    FuriThread* render_thread;
    FuriMutex* mutex;

    esp_lcd_panel_handle_t panel;
    uint16_t w;
    uint16_t h;
    uint16_t* fb; // PSRAM, w*h RGB565 (byte-swapped)
    uint16_t* stripe; // DRAM DMA, w*STRIPE_H
    uint8_t* wf; // PSRAM, WF_ROWS_MAX*BINS normalized power (0..255)

    atomic_bool running;
    atomic_bool dirty;

    // model (mutex-protected)
    float rssi[BINS];
    float peak[BINS];
    float displayed[BINS]; // visual easing only; waterfall/readouts remain raw
    uint32_t center;
    uint32_t span;
    uint16_t cursor;
    SubGhzSpectrumAdjust adjust;
    bool has_data;
    uint16_t wf_write;
    uint16_t dwell_ms;
    int16_t threshold;
    bool paused;
    bool peak_hold;
    bool peak_decay;
    bool spectrogram;
    bool configured_window;
    uint16_t bins;
};

/* ---- 5x7 font (column-major, bit0 = top row) ---------------------------- */
typedef struct {
    char c;
    uint8_t col[5];
} SubGhzGlyph;

static const SubGhzGlyph subghz_font[] = {
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}}, {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}}, {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}}, {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}}, {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}}, {'m', {0x7C, 0x04, 0x18, 0x04, 0x78}},
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
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
#define COL_BODY   rgb565(70, 13, 11)
#define COL_BODY_PEAK rgb565(135, 28, 18)
#define COL_HOLD   rgb565(0, 210, 210)
#define COL_CURSOR rgb565(255, 220, 0)
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
    } else if(dec == 2) {
        snprintf(buf, n, "%lu.%02lu", mhz, (unsigned long)((hz % 1000000UL) / 10000UL));
    } else {
        snprintf(buf, n, "%lu.%03lu", mhz, (unsigned long)((hz % 1000000UL) / 1000UL));
    }
}

/* ---- frame composition (holds the data mutex) --------------------------- */
static float subghz_waterfall_norm(float rssi) {
    float value = (rssi + 100.0f) / 70.0f;
    return value < 0 ? 0 : value > 1 ? 1 : value;
}

/* Bruce-beta workflow has its own plot geometry and controls. The local CC1101
 * service remains shared; this is not the legacy spectrum with a palette swap. */
static void subghz_spectrogram_compose(SubGhzSpectrum* s) {
    const int left = 8, width = s->w - 16;
    const int footer = s->h - 16, ruler = footer - 11;
    const int top = 20, bottom = s->h / 2, height = bottom - top;
    const int waterfall_top = bottom + 3;
    const int rows = ruler - waterfall_top - 2;
    memset(s->fb, 0, (size_t)s->w * s->h * sizeof(*s->fb));
    fb_text(s, left, 5, "RF WATERFALL", COL_TRACE, 1);
    fb_text(s, s->w - 86, 5, s->paused ? "OK RESUME" : "OK PAUSE", COL_TEXT_DIM, 1);
    unsigned strongest = 0;
    for(unsigned i = 1; i < s->bins; ++i)
        if(s->rssi[i] > s->rssi[strongest]) strongest = i;
    int strongest_column = strongest * (width - 1) / (s->bins - 1);
    for(int column = 0; column < width; ++column) {
        int live_y = bottom, peak_y = bottom;
        if(s->has_data) {
            live_y -= subghz_waterfall_norm(subghz_spectrum_interpolate(s->displayed, s->bins, column, width)) * height;
            peak_y -= subghz_waterfall_norm(subghz_spectrum_interpolate(s->peak, s->bins, column, width)) * height;
        }
        bool highlight = column >= strongest_column - width / 40 && column <= strongest_column + width / 40;
        /* A restrained blue glow sits under the real red trace. It changes only
         * presentation, never the measured RSSI or the waterfall samples. */
        fb_vline(s, left + column, live_y, bottom, highlight ? rgb565(4,16,42) : rgb565(3,9,24));
        for(int depth = 1; depth <= 6 && live_y + depth <= bottom; ++depth)
            fb_px(s, left + column, live_y + depth, rgb565(0,100-depth*10,200-depth*20));
        fb_px(s, left + column, live_y, COL_TRACE);
        if(s->has_data && peak_y < live_y) fb_px(s, left + column, peak_y, COL_HOLD);
        if((column & 3) == 0)
            for(int quarter = 1; quarter <= 3; ++quarter) {
                int y = bottom - height * quarter / 4;
                if(y < live_y - 1) fb_px(s, left + column, y, COL_GRID);
            }
    }
    fb_hline(s, left, left + width - 1, bottom + 1, rgb565(0,35,90));
    fb_hline(s, left, left + width - 1, bottom + 2, rgb565(0,12,35));
    for(int age = 0; age < rows; ++age) {
        const uint8_t* row = s->wf + subghz_spectrum_history_row(s->wf_write, age, WF_ROWS_MAX) * BINS;
        for(int column = 0; column < width; ++column) {
            unsigned position = (unsigned)column * (s->bins - 1) * 256 / (width - 1);
            unsigned bin = position / 256, fraction = position % 256;
            unsigned level = row[bin];
            if(bin + 1 < s->bins) level = (level * (256 - fraction) + row[bin + 1] * fraction) / 256;
            uint16_t color = subghz_spectrum_sdr_color(level);
            fb_px(s, left + column, waterfall_top + age, (uint16_t)((color << 8) | (color >> 8)));
        }
    }
    char label[64];
    uint32_t start = s->center - s->span / 2;
    for(unsigned tick = 0; tick < 5; ++tick) {
        fmt_mhz(start + (uint64_t)s->span * tick / 4, label, sizeof(label), 2);
        int x = left + tick * (width - 1) / 4 - text_width(label, 1) / 2;
        if(x < left) x = left;
        if(x + text_width(label, 1) > left + width) x = left + width - text_width(label, 1);
        fb_text(s, x, ruler, label, COL_TEXT_DIM, 1);
    }
    if(s->has_data && s->rssi[strongest] > -127.0f) {
        char freq[16];
        fmt_mhz(subghz_spectrum_bin_frequency(s->center, s->span, strongest, s->bins, true), freq, sizeof(freq), 3);
        snprintf(label, sizeof(label), "%ddBm %sMHz  ROTATE PAN", (int)s->rssi[strongest], freq);
    } else {
        snprintf(label, sizeof(label), "NO DATA  ROTATE PAN");
    }
    fb_text(s, left, footer, label, COL_TEXT, 1);
}

static void subghz_spectrum_compose(SubGhzSpectrum* s) {
    if(s->spectrogram) { subghz_spectrogram_compose(s); return; }
    const int w = s->w;
    const int h = s->h;
    const int spec_h = (h * 60) / 100;
    const int plot_top = 34; // title, raw measurement and parameter rows
    const int plot_bottom = spec_h - 10; // reserve bottom strip for freq ticks
    const int plot_h = plot_bottom - plot_top;
    const int wf_h = h - spec_h - 10; // footer identifies hold mode / pause

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

    unsigned peak_bin = 0;
    for(unsigned i = 1; i < BINS; ++i)
        if(s->rssi[i] > s->rssi[peak_bin]) peak_bin = i;

    // --- traces (visual interpolation, never extra measurement bins) ---
    if(s->has_data) {
        const float* live = s->spectrogram ? s->displayed : s->rssi;
        int previous_hold = 0, previous_live = 0;
        int peak_column = peak_bin * (w - 1) / (BINS - 1);
        for(int x = 0; x < w; x++) {
            int yh = plot_bottom - (int)(subghz_norm(subghz_spectrum_interpolate(s->peak, BINS, x, w)) * plot_h);
            int yl = plot_bottom - (int)(subghz_norm(subghz_spectrum_interpolate(live, BINS, x, w)) * plot_h);
            if(s->spectrogram) {
                bool highlight = x >= peak_column - w / 40 && x <= peak_column + w / 40;
                fb_vline(s, x, yl, plot_bottom, highlight ? COL_BODY_PEAK : COL_BODY);
            }
            if(x > 0) {
                if(s->peak_hold || s->peak_decay) fb_line(s, x - 1, previous_hold, x, yh, COL_HOLD);
                fb_line(s, x - 1, previous_live, x, yl, COL_TRACE);
            }
            previous_hold = yh;
            previous_live = yl;
        }
    }

    // --- cursor ---
    int cx = s->cursor * (w - 1) / (BINS - 1);
    fb_vline(s, cx, plot_top, plot_bottom, COL_CURSOR);
    int threshold_y = plot_bottom - (int)(subghz_norm(s->threshold) * plot_h);
    fb_hline(s, 0, w - 1, threshold_y, COL_TEXT_DIM);

    // --- waterfall ---
    for(int r = 0; r < wf_h && r < WF_ROWS_MAX; r++) {
        size_t history_row = subghz_spectrum_history_row(s->wf_write, r, WF_ROWS_MAX);
        uint8_t* row = &s->wf[history_row * BINS];
        int y = spec_h + r;
        for(int x = 0; x < w; x++) {
            unsigned position = (unsigned)x * (BINS - 1) * 256 / (w - 1);
            unsigned bin = position / 256;
            unsigned fraction = position % 256;
            unsigned level = row[bin];
            if(bin + 1 < BINS) level = (row[bin] * (256 - fraction) + row[bin + 1] * fraction) / 256;
            uint16_t color = subghz_spectrum_sdr_color((uint8_t)level);
            color = (uint16_t)((color << 8) | (color >> 8));
            fb_px(s, x, y, s->spectrogram ? color : magma(level / 255.0f));
        }
    }
    // cursor column marker in the waterfall
    fb_vline(s, cx, spec_h, spec_h + wf_h - 1, COL_CURSOR);

    // --- text: readout ---
    char buf[64];
    const char* tags[] = {"CUR", "CEN", "SPN", "STEP", "DWL", "THR", "HOLD", "RUN"};
    const char* tag = tags[s->adjust];
    fb_text(s, 2, 2, s->spectrogram ? "SPECTROGRAM" : "SPECTRUM", COL_TEXT, 1);
    fb_text(s, 92, 2, tag, COL_CURSOR, 1);

    char fbuf[16];
    fmt_mhz(cursor_freq, fbuf, sizeof(fbuf), 3);
    if(s->has_data) {
        snprintf(buf, sizeof(buf), "CUR %s %ddBm", fbuf, (int)s->rssi[s->cursor]);
    } else {
        snprintf(buf, sizeof(buf), "CUR %s", fbuf);
    }
    fb_text(s, 2, 12, buf, COL_TEXT, 1);
    if(s->has_data && s->rssi[peak_bin] > -127.0f) {
        fmt_mhz(f_start + peak_bin * stepf + stepf / 2, fbuf, sizeof(fbuf), 3);
        snprintf(buf, sizeof(buf), "PK %s %ddBm", fbuf, (int)s->rssi[peak_bin]);
        fb_text(s, w - text_width(buf, 1) - 2, 12, buf, COL_HOLD, 1);
    } else {
        fb_text(s, w - 44, 12, "PK NONE", COL_TEXT_DIM, 1);
    }
    snprintf(buf, sizeof(buf), "STEP %luHz DWL %u THR %d %s",
        (unsigned long)stepf, s->dwell_ms, s->threshold, s->paused ? "PAUSE" : "RUN");
    fb_text(s, 2, 23, buf, COL_TEXT_DIM, 1);
    snprintf(buf, sizeof(buf), "HOLD %s  %s",
        s->peak_hold ? "HOLD" : s->peak_decay ? "DECAY" : "OFF", s->paused ? "PAUSE" : "RUN");
    fb_text(s, 2, h - 8, buf, COL_TEXT_DIM, 1);

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
    const int w = s->w;
    const int h = s->h;
    for(int y0 = 0; y0 < h; y0 += STRIPE_H) {
        int rows = (y0 + STRIPE_H > h) ? (h - y0) : STRIPE_H;
        memcpy(s->stripe, &s->fb[(size_t)y0 * w], (size_t)rows * w * sizeof(uint16_t));
        if(!furi_hal_display_draw_rgb565(0, y0, w, rows, s->stripe)) break;
    }
}

static int32_t subghz_spectrum_render_thread(void* context) {
    SubGhzSpectrum* s = context;

    s->gui = furi_record_open(RECORD_GUI);
    gui_direct_draw_acquire(s->gui);
    s->panel = furi_hal_display_get_panel_handle();

    while(s->running) {
        if(s->dirty && s->panel && s->fb && s->stripe && s->wf) {
            furi_mutex_acquire(s->mutex, FuriWaitForever);
            s->dirty = false;
            subghz_spectrum_compose(s);
            furi_mutex_release(s->mutex);
            subghz_spectrum_blit(s);
        }
        furi_delay_ms(25);
    }

    gui_direct_draw_release(s->gui);
    furi_record_close(RECORD_GUI);
    s->gui = NULL;
    return 0;
}

/* ---- input (runs on the input pubsub thread) ---------------------------- */
static void subghz_spectrum_reset_data(SubGhzSpectrum* s) {
    for(int i = 0; i < BINS; i++) {
        s->rssi[i] = s->spectrogram ? -100.0f : RSSI_FLOOR;
        s->peak[i] = s->rssi[i];
        s->displayed[i] = s->rssi[i];
    }
    memset(s->wf, 0, (size_t)WF_ROWS_MAX * BINS);
    s->wf_write = 0;
    s->has_data = false;
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

    // Back (side key short, or encoder long) -> exit
    if(event->key == InputKeyBack && event->type == InputTypePress) {
        s->running = false;
        if(s->callback) s->callback(SubGhzCustomEventViewSpectrumBack, s->context);
        return;
    }

    if(s->spectrogram) {
        bool toggle = event->key == InputKeyOk && event->type == InputTypeShort;
        bool direction = (event->type == InputTypePress || event->type == InputTypeRepeat) &&
            (event->key == InputKeyUp || event->key == InputKeyDown || event->key == InputKeyLeft || event->key == InputKeyRight);
        if(!toggle && !direction) return;
        furi_mutex_acquire(s->mutex, FuriWaitForever);
        if(toggle) s->paused = !s->paused;
        if(direction) {
            int sign = event->key == InputKeyDown || event->key == InputKeyRight ? 1 : -1;
            int64_t center = (int64_t)s->center + sign * (int64_t)subghz_spectrum_pan_step(s->span);
            int64_t low = CENTER_MIN + s->span / 2, high = CENTER_MAX - (s->span - s->span / 2);
            if(center < low) center = low;
            if(center > high) center = high;
            s->center = center;
            subghz_spectrum_reset_data(s); // never mix different frequency axes in history
        }
        uint32_t center = s->center, span = s->span;
        bool paused = s->paused;
        s->dirty = true;
        furi_mutex_release(s->mutex);
        if(direction) subghz_spectrum_worker_set_window(s->worker, center, span);
        if(toggle) subghz_spectrum_worker_set_controls(s->worker, paused, s->dwell_ms);
        return;
    }

    // OK short -> cycle the active control
    if(event->key == InputKeyOk && event->type == InputTypeShort) {
        furi_mutex_acquire(s->mutex, FuriWaitForever);
        s->adjust = (s->adjust + 1) % SubGhzSpectrumAdjustMAX;
        s->dirty = true;
        furi_mutex_release(s->mutex);
        return;
    }

    bool move = (event->type == InputTypePress) || (event->type == InputTypeRepeat);
    bool is_dir = (event->key == InputKeyUp) || (event->key == InputKeyDown) ||
                  (event->key == InputKeyLeft) || (event->key == InputKeyRight);
    if(!move || !is_dir) return;

    int dir = (event->key == InputKeyDown || event->key == InputKeyRight) ? 1 : -1;
    bool changed_window = false;
    bool changed_controls = false;
    uint32_t new_center = 0, new_span = 0;

    furi_mutex_acquire(s->mutex, FuriWaitForever);
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
    case SubGhzSpectrumAdjustStep: {
        int64_t span = (int64_t)s->span + dir * (int64_t)BINS * 2500;
        if(span < 128000) span = 128000;
        if(span > 16000000) span = 16000000;
        s->span = (uint32_t)span;
        subghz_spectrum_reset_data(s);
        changed_window = true;
        break;
    }
    case SubGhzSpectrumAdjustDwell:
        if(dir > 0 && s->dwell_ms < 20) ++s->dwell_ms;
        if(dir < 0 && s->dwell_ms > 1) --s->dwell_ms;
        changed_controls = true;
        break;
    case SubGhzSpectrumAdjustThreshold:
        s->threshold += dir;
        if(s->threshold < -95) s->threshold = -95;
        if(s->threshold > -30) s->threshold = -30;
        break;
    case SubGhzSpectrumAdjustHold:
        /* OFF -> DECAY (beta behavior) -> HOLD, bounded in both directions. */
        if(dir > 0) {
            if(s->peak_decay) { s->peak_hold = true; s->peak_decay = false; }
            else if(!s->peak_hold) s->peak_decay = true;
        } else {
            if(s->peak_hold) { s->peak_hold = false; s->peak_decay = true; }
            else s->peak_decay = false;
        }
        for(int i = 0; i < BINS; ++i) s->peak[i] = s->rssi[i];
        break;
    case SubGhzSpectrumAdjustRun:
        s->paused = dir < 0;
        changed_controls = true;
        break;
    default:
        break;
    }
    new_center = s->center;
    new_span = s->span;
    bool paused = s->paused;
    uint16_t dwell_ms = s->dwell_ms;
    s->dirty = true;
    furi_mutex_release(s->mutex);

    if(changed_window) subghz_spectrum_worker_set_window(s->worker, new_center, new_span);
    if(changed_controls) subghz_spectrum_worker_set_controls(s->worker, paused, dwell_ms);
}

/* ---- worker sweep callback (runs on the worker thread) ------------------ */
static void subghz_spectrum_sweep_callback(
    void* context,
    const float* rssi,
    uint32_t center,
    uint32_t span,
    uint16_t bins) {
    SubGhzSpectrum* s = context;
    furi_mutex_acquire(s->mutex, FuriWaitForever);
    if(s->wf && !s->paused && s->center == center && s->span == span && bins == s->bins) {
        uint8_t* row = &s->wf[s->wf_write * BINS];
        for(int i = 0; i < bins; i++) {
            s->rssi[i] = rssi[i];
            s->displayed[i] += (rssi[i] - s->displayed[i]) / 3.0f;
            if(s->peak_decay) s->peak[i] -= s->spectrogram ? 0.7f : 0.65f;
            if(!s->peak_hold && !s->peak_decay) s->peak[i] = rssi[i];
            if(rssi[i] > s->peak[i]) s->peak[i] = rssi[i];
            row[i] = (uint8_t)((s->spectrogram ? subghz_waterfall_norm(rssi[i]) : subghz_norm(rssi[i])) * 255.0f);
        }
        s->wf_write = (s->wf_write + 1) % WF_ROWS_MAX;
        s->has_data = true;
        s->dirty = true;
    }
    furi_mutex_release(s->mutex);
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
    if(instance->running) return true;

    instance->w = furi_hal_display_get_h_res();
    instance->h = furi_hal_display_get_v_res();

    instance->fb =
        heap_caps_malloc((size_t)instance->w * instance->h * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    instance->wf = heap_caps_malloc((size_t)WF_ROWS_MAX * BINS, MALLOC_CAP_SPIRAM);
    instance->stripe =
        heap_caps_malloc((size_t)instance->w * STRIPE_H * sizeof(uint16_t), MALLOC_CAP_DMA);
    if(!instance->fb || !instance->wf || !instance->stripe) {
        subghz_spectrum_stop(instance);
        return false;
    }

    if(!instance->configured_window) {
        instance->center = instance->spectrogram ? 434000000UL : 433920000UL;
        instance->span = instance->spectrogram ? 2000000UL : 6000000UL;
    }
    instance->bins = instance->spectrogram ? SUBGHZ_WATERFALL_BINS : BINS;
    instance->cursor = instance->bins / 2;
    instance->adjust = SubGhzSpectrumAdjustCursor;
    instance->paused = false;
    instance->peak_hold = !instance->spectrogram;
    instance->peak_decay = instance->spectrogram;
    instance->threshold = -70;
    instance->dwell_ms = 3;
    subghz_spectrum_reset_data(instance);
    instance->dirty = true;

    // Worker
    instance->worker = subghz_spectrum_worker_alloc();
    if(!instance->worker) { subghz_spectrum_stop(instance); return false; }
    subghz_spectrum_worker_set_spectrogram(instance->worker, instance->spectrogram);
    subghz_spectrum_worker_set_window(instance->worker, instance->center, instance->span);
    subghz_spectrum_worker_set_controls(instance->worker, false, instance->dwell_ms);
    subghz_spectrum_worker_set_callback(
        instance->worker, subghz_spectrum_sweep_callback, instance);

    // Input
    instance->input = furi_record_open(RECORD_INPUT_EVENTS);
    instance->input_sub =
        furi_pubsub_subscribe(instance->input, subghz_spectrum_input_callback, instance);

    // Render thread (takes over the display)
    instance->running = true;
    instance->render_thread = furi_thread_alloc_ex(
        "SubGhzSpectrumRender", 4096, subghz_spectrum_render_thread, instance);
    furi_thread_start(instance->render_thread);

    subghz_spectrum_worker_start(instance->worker);
    return true;
}

void subghz_spectrum_stop(SubGhzSpectrum* instance) {
    furi_assert(instance);
    instance->running = false;
    /* Drain input callbacks before destroying the worker they configure. */
    if(instance->input_sub) {
        furi_pubsub_unsubscribe(instance->input, instance->input_sub);
        instance->input_sub = NULL;
    }

    // Stop sweeps first (frees the SPI bus for the final render teardown).
    if(instance->worker && subghz_spectrum_worker_is_running(instance->worker)) {
        subghz_spectrum_worker_stop(instance->worker);
    }

    // Stop the render loop; the thread releases direct-draw before it returns.
    instance->running = false;
    if(instance->render_thread) {
        furi_thread_join(instance->render_thread);
        furi_thread_free(instance->render_thread);
        instance->render_thread = NULL;
    }

    if(instance->input) {
        furi_record_close(RECORD_INPUT_EVENTS);
        instance->input = NULL;
    }

    if(instance->worker) {
        subghz_spectrum_worker_free(instance->worker);
        instance->worker = NULL;
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

/* Blank placeholder view — the color frame is drawn via direct display
 * takeover, so this view only exists to keep the ViewDispatcher happy. */
static void subghz_spectrum_view_draw(Canvas* canvas, void* model) {
    UNUSED(model);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "RF Spectrum");
}

SubGhzSpectrum* subghz_spectrum_alloc(void) {
    SubGhzSpectrum* instance = heap_caps_calloc(1, sizeof(SubGhzSpectrum), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    furi_check(instance);
    memset(instance, 0, sizeof(SubGhzSpectrum));
    instance->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    instance->view = view_alloc();
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, subghz_spectrum_view_draw);
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

void subghz_spectrum_set_spectrogram(SubGhzSpectrum* instance, bool enabled) {
    furi_assert(instance);
    furi_assert(!instance->running);
    instance->spectrogram = enabled;
}

bool subghz_spectrum_configure_window(SubGhzSpectrum* instance, uint32_t start, uint32_t end) {
    furi_assert(instance && !instance->running);
    if(start < CENTER_MIN || end > CENTER_MAX || end <= start || end - start < 1000 ||
       !subghz_spectrum_frequency_supported(start) || !subghz_spectrum_frequency_supported(end)) return false;
    instance->span = end - start;
    instance->center = start + instance->span / 2;
    instance->configured_window = true;
    return true;
}
