#include "wlan_deauth_all_view.h"
#include "wlan_view_common.h"
#include "wlan_view_events.h"
#include "wlan_deauth_all_icon.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <input/input.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// LAYOUT KONSTAN — dua panel: skull (kiri) + info (kanan)
// ----------------------------------------------------------------------------
// Layar 128x64. Panel kiri menampung skull 28x48 (SKULL_X..SKULL_X+SKULL_W),
// garis pemisah vertikal di DIVIDER_X, lalu panel info di kanan. Semua teks
// kanan dipusatkan pada RIGHT_CX dan dijaga di dalam [RIGHT_X0..RIGHT_X1]
// supaya tidak mepet/menabrak skull maupun terpotong tepi layar.
// ============================================================================
#define DIVIDER_X     33
#define RIGHT_X0      36
#define RIGHT_X1      127
#define RIGHT_CX      ((RIGHT_X0 + RIGHT_X1) / 2) // 81
#define RIGHT_W       (RIGHT_X1 - RIGHT_X0)       // 91

// Format pencacah jadi maksimal ~5 char ("9999", "9999k", "4294M") supaya baris
// metrik tidak pernah melebihi lebar panel kanan walau sesi flooding panjang
// (frames uint32 bisa membengkak). buf minimal 7 byte.
static void fmt_count(char* buf, size_t sz, unsigned long v) {
    if(v < 10000UL) {
        snprintf(buf, sz, "%lu", v);
    } else if(v < 10000000UL) {
        snprintf(buf, sz, "%luk", v / 1000UL);
    } else {
        snprintf(buf, sz, "%luM", v / 1000000UL);
    }
}

// ============================================================================
// DRAW CALLBACK
// ============================================================================
static void wlan_deauth_all_view_draw_callback(Canvas* canvas, void* context) {
    WlanDeauthAllModel* model = context;
    char line[64];

    canvas_clear(canvas);

    // ------------------------------------------------------------------------
    // Panel kiri: skull + garis pemisah vertikal
    // ------------------------------------------------------------------------
    canvas_draw_xbm(canvas, SKULL_X, SKULL_Y, SKULL_W, SKULL_H, skull_28x48_bits);
    canvas_draw_line(canvas, DIVIDER_X, 4, DIVIDER_X, 59);

    // ------------------------------------------------------------------------
    // Panel kanan — judul kiri-atas + garis bawah judul
    // ------------------------------------------------------------------------
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, RIGHT_X0 + 2, 9, AlignLeft, AlignBottom, "Deauth All");
    canvas_draw_line(canvas, RIGHT_X0, 11, RIGHT_X1 - 1, 11);

    // ------------------------------------------------------------------------
    // Baris status (FontSecondary, dipusatkan di panel kanan)
    // ------------------------------------------------------------------------
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas, RIGHT_CX, 25, AlignCenter, AlignBottom,
        model->running ? "[ FLOODING ]" : "SYSTEM READY");

    // ------------------------------------------------------------------------
    // Baris detail: status string / SSID / AP count
    // ------------------------------------------------------------------------
    const char* status_str =
        (model->target_ssid[0] != '\0') ? model->target_ssid : "-- idle --";
    canvas_draw_str_aligned(canvas, RIGHT_CX, 36, AlignCenter, AlignBottom, status_str);

    // ------------------------------------------------------------------------
    // Baris metrik: channel + paket + waktu digabung jadi satu baris
    // ------------------------------------------------------------------------
    if(model->running) {
        char pk[8];
        fmt_count(pk, sizeof(pk), (unsigned long)model->frames);
        snprintf(line, sizeof(line), "CH%u %sp %lus",
                 model->current_channel,
                 pk,
                 (unsigned long)(model->elapsed_ms / 1000U));
    } else {
        snprintf(line, sizeof(line), "Hop CH 1-13");
    }
    canvas_draw_str_aligned(canvas, RIGHT_CX, 47, AlignCenter, AlignBottom, line);

    // ------------------------------------------------------------------------
    // Tombol aksi: frame rounded di panel kanan (tidak menabrak skull kiri)
    // ------------------------------------------------------------------------
    const char* btn = model->running ? "OK  Stop" : "OK  Attack All";
    uint16_t tw = canvas_string_width(canvas, btn);
    uint8_t bw = (uint8_t)(tw + 8);
    if(bw > RIGHT_W) bw = RIGHT_W;
    uint8_t bx = (uint8_t)(RIGHT_X0 + (RIGHT_W - bw) / 2);
    uint8_t by = 52;
    uint8_t bh = 11;
    elements_slightly_rounded_frame(canvas, bx, by, bw, bh);
    canvas_draw_str_aligned(
        canvas, bx + bw / 2, by + bh / 2 + 1, AlignCenter, AlignCenter, btn);
}

// ============================================================================
// INPUT CALLBACK
// ----------------------------------------------------------------------------
// - OK    → kirim custom event DeauthAllToggle
// - Back  → return false supaya scene manager pop scene
// ============================================================================
static bool wlan_deauth_all_view_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* dispatcher = context;

    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        view_dispatcher_send_custom_event(
            dispatcher, WlanAppCustomEventDeauthAllToggle);
        return true;
    }

    // Back dan tombol lain: biarkan scene manager handle
    return false;
}

// ============================================================================
// ALLOC / FREE
// ============================================================================
View* wlan_deauth_all_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(WlanDeauthAllModel));
    view_set_draw_callback(view, wlan_deauth_all_view_draw_callback);
    view_set_input_callback(view, wlan_deauth_all_view_input_callback);
    return view;
}

void wlan_deauth_all_view_free(View* view) {
    view_free(view);
}
