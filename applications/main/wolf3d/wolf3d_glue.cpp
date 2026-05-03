/* wolf3d_glue.cpp — Display-Takeover, Stripe-Buffer-Pipeline, Splash,
 * longjmp-basiertes Quit-Handling.
 *
 * Eng angelehnt an applications/main/doom/doomgeneric_flipper.c, übersetzt
 * für Wolf3Ds 8-bit-indexed-Pipeline (Doom hatte schon RGBA8888).
 */

#include "wolf3d_glue.h"

#include <string.h>
#include <setjmp.h>

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_display.h>
#include <furi_hal_spi_bus.h>
#include <gui/gui.h>

#include <esp_lcd_panel_ops.h>
#include <esp_heap_caps.h>

#define TAG "Wolf3D"

namespace {

Gui*                    s_gui = nullptr;
Canvas*                 s_canvas = nullptr;
FuriPubSub*             s_input_events = nullptr;
esp_lcd_panel_handle_t  s_panel = nullptr;
uint16_t                s_panel_w = 0;
uint16_t                s_panel_h = 0;

/* DMA-fähiger Stripe-Buffer (RGB565, 16 Zeilen ≈ 10 KB). */
constexpr int STRIPE_H = 16;
uint16_t* s_rgb_stripe = nullptr;

/* Aktive 8-bit-Palette (256 Einträge × 3 Bytes = 768 B). */
uint8_t s_palette_rgb[256 * 3];

/* longjmp-Buffer, in den Wolf4SDLs Quit() springt. */
jmp_buf s_quit_jmp;
bool    s_quit_jmp_set = false;

inline uint16_t rgb565_swap(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = static_cast<uint16_t>(((r & 0xF8) << 8) |
                                       ((g & 0xFC) << 3) |
                                       (b >> 3));
    return static_cast<uint16_t>((c >> 8) | (c << 8)); /* ST7789 wants big-endian */
}

} /* namespace */

extern "C" void wolf3d_input_init(FuriPubSub* input_events);
extern "C" void wolf3d_input_deinit(void);

extern "C" {

void wolf3d_glue_setup(Gui* gui, FuriPubSub* input_events) {
    s_gui = gui;
    s_input_events = input_events;
    s_canvas = gui_direct_draw_acquire(gui);
    wolf3d_input_init(input_events);

    s_panel = furi_hal_display_get_panel_handle();
    s_panel_w = furi_hal_display_get_h_res();
    s_panel_h = furi_hal_display_get_v_res();

    size_t bytes = static_cast<size_t>(s_panel_w) * STRIPE_H * sizeof(uint16_t);
    s_rgb_stripe = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_DMA));
    if(!s_rgb_stripe) {
        FURI_LOG_E(TAG, "stripe alloc failed (%u bytes)", static_cast<unsigned>(bytes));
    }

    memset(s_palette_rgb, 0, sizeof(s_palette_rgb));

    FURI_LOG_I(TAG, "glue ready: %ux%u panel, stripe=%p", s_panel_w, s_panel_h, s_rgb_stripe);
}

void wolf3d_glue_teardown(void) {
    wolf3d_input_deinit();
    if(s_rgb_stripe) {
        heap_caps_free(s_rgb_stripe);
        s_rgb_stripe = nullptr;
    }
    if(s_canvas && s_gui) {
        gui_direct_draw_release(s_gui);
        s_canvas = nullptr;
    }
}

void wolf3d_glue_show_splash(void) {
    /* TODO: PNG → RGB565-C-Array wie doom_splash.c. Vorerst: schwarz. */
    if(!s_panel || !s_rgb_stripe) return;
    memset(s_rgb_stripe, 0, static_cast<size_t>(s_panel_w) * STRIPE_H * sizeof(uint16_t));
    furi_hal_spi_bus_lock();
    for(int y = 0; y < s_panel_h; y += STRIPE_H) {
        int rows = (y + STRIPE_H > s_panel_h) ? (s_panel_h - y) : STRIPE_H;
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, s_panel_w, y + rows, s_rgb_stripe);
    }
    furi_hal_spi_bus_unlock();
}

int wolf3d_glue_set_quit_jmp(void) {
    s_quit_jmp_set = true;
    return setjmp(s_quit_jmp);
}

void wolf3d_glue_do_quit(void) {
    if(s_quit_jmp_set) {
        longjmp(s_quit_jmp, 1);
    }
    /* Wenn Quit() vor set_quit_jmp gerufen wird (sollte nicht passieren),
     * geben wir wenigstens kontrolliert auf. */
    FURI_LOG_E(TAG, "Quit() before jmp set — restarting");
    esp_restart();
    while(1) {}
}

void wolf3d_glue_set_palette(const uint8_t* rgb_triplets, int first, int count) {
    if(!rgb_triplets || first < 0 || first >= 256) return;
    int n = count;
    if(first + n > 256) n = 256 - first;
    memcpy(&s_palette_rgb[first * 3], rgb_triplets, static_cast<size_t>(n) * 3);
}

void wolf3d_glue_present_indexed(const uint8_t* pixels,
                                 int width, int height, int pitch) {
    if(!s_panel || !s_rgb_stripe || !pixels) return;

    const uint16_t panel_w = s_panel_w;
    const uint16_t panel_h = s_panel_h;
    const uint16_t game_w = static_cast<uint16_t>(width);
    const uint16_t game_h = static_cast<uint16_t>(height);

    furi_hal_spi_bus_lock();
    for(uint16_t y0 = 0; y0 < panel_h; y0 += STRIPE_H) {
        uint16_t rows = (y0 + STRIPE_H > panel_h) ? (panel_h - y0) : STRIPE_H;
        for(uint16_t row = 0; row < rows; row++) {
            uint16_t panel_y = y0 + row;
            /* Vertical nearest-neighbour: panel_y → game_y */
            uint16_t game_y = static_cast<uint16_t>(
                (static_cast<uint32_t>(panel_y) * game_h + panel_h / 2) / panel_h);
            if(game_y >= game_h) game_y = game_h - 1;
            const uint8_t* src_row = pixels + static_cast<uint32_t>(game_y) * pitch;
            uint16_t* dst_row = &s_rgb_stripe[row * panel_w];
            uint16_t copy_w = (game_w < panel_w) ? game_w : panel_w;
            for(uint16_t x = 0; x < copy_w; x++) {
                uint8_t idx = src_row[x];
                const uint8_t* p = &s_palette_rgb[idx * 3];
                dst_row[x] = rgb565_swap(p[0], p[1], p[2]);
            }
            for(uint16_t x = copy_w; x < panel_w; x++) dst_row[x] = 0;
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, y0, panel_w, y0 + rows, s_rgb_stripe);
    }
    furi_hal_spi_bus_unlock();
}

uint32_t wolf3d_glue_ticks_ms(void) {
    return static_cast<uint32_t>(furi_get_tick()) * (1000u / configTICK_RATE_HZ);
}

void wolf3d_glue_delay_ms(uint32_t ms) {
    furi_delay_ms(ms);
}

} /* extern "C" */
