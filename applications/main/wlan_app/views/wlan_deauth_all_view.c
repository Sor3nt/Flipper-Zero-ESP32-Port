#include "wlan_deauth_all_view.h"
#include "wlan_view_common.h"
#include "wlan_view_events.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <input/input.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// LAYOUT KONSTAN — satu baris kontrol dengan urutan fokus kiri ke kanan:
//   Start/Stop -> Auto Scan -> Scan timer.
// ----------------------------------------------------------------------------
// Layar 128x64:
//   y<=12 : judul "Deauth All" + garis bawah
//   y=26  : baris status (target_ssid, center)
//   y=38  : baris metrik (center)
//   y=50.. : [ Start/Stop ] [ Auto Scan ] [ Scan timer ]
// ============================================================================
#define SCREEN_W      128
#define CONTROL_ROW_Y 50
#define BTN_H         10
#define BTN_PAD       6  // padding teks dalam tombol (3px kiri + 3px kanan)
#define BTN_GAP       4
#define CONTROL_MARGIN 3

// Format pencacah jadi maksimal ~5 char ("9999", "9999k", "4294M").
static void fmt_count(char* buf, size_t sz, unsigned long v) {
    if(v < 10000UL) {
        snprintf(buf, sz, "%lu", v);
    } else if(v < 10000000UL) {
        snprintf(buf, sz, "%luk", v / 1000UL);
    } else {
        snprintf(buf, sz, "%luM", v / 1000000UL);
    }
}

// Cincin fokus: frame membulat 2px di luar kotak kontrol. Canvas meng-clip jika
// mepet tepi, jadi aman walau kontrol kanan menyentuh edge.
static void draw_focus_ring(Canvas* canvas, int x, int y, int w, int h) {
    int fx = x - 2;
    int fy = y - 2;
    if(fx < 0) fx = 0;
    if(fy < 0) fy = 0;
    elements_slightly_rounded_frame(canvas, fx, fy, w + 4, h + 4);
}

// Tombol aksi. filled=true → kotak hitam penuh + teks putih (dipakai Auto Scan
// saat AKTIF: tombol jadi hitam, teksnya transparan/putih). filled=false →
// frame garis saja + teks hitam (transparan kecuali teks).
static void draw_action_button(
    Canvas* canvas, int x, int y, int w, int h, const char* text, bool filled, bool focused) {
    if(filled) {
        canvas_draw_rbox(canvas, x, y, w, h, 2);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str_aligned(canvas, x + w / 2, y + h / 2 + 1, AlignCenter, AlignCenter, text);
        canvas_set_color(canvas, ColorBlack);
    } else {
        elements_slightly_rounded_frame(canvas, x, y, w, h);
        canvas_draw_str_aligned(canvas, x + w / 2, y + h / 2 + 1, AlignCenter, AlignCenter, text);
    }
    if(focused) draw_focus_ring(canvas, x, y, w, h);
}

// ============================================================================
// DRAW CALLBACK
// ============================================================================
static void wlan_deauth_all_view_draw_callback(Canvas* canvas, void* context) {
    WlanDeauthAllModel* model = context;
    char line[64];

    canvas_clear(canvas);

    // --- Judul + garis bawah -------------------------------------------------
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 2, 10, AlignLeft, AlignBottom, "Deauth All");
    canvas_draw_line(canvas, 0, 12, SCREEN_W - 1, 12);

    canvas_set_font(canvas, FontSecondary);

    // --- Baris status (target_ssid diisi oleh scene: READY/SCANNING/FLOOD/...) -
    const char* status_str =
        (model->target_ssid[0] != '\0') ? model->target_ssid : "SYSTEM READY";
    canvas_draw_str_aligned(canvas, SCREEN_W / 2, 26, AlignCenter, AlignBottom, status_str);

    // --- Baris metrik --------------------------------------------------------
    if(model->running) {
        char pk[8];
        fmt_count(pk, sizeof(pk), (unsigned long)model->frames);
        snprintf(line, sizeof(line), "CH%u %sp %lus",
                 model->current_channel, pk, (unsigned long)(model->elapsed_ms / 1000U));
    } else {
        snprintf(line, sizeof(line), "Hop CH 1-13");
    }
    canvas_draw_str_aligned(canvas, SCREEN_W / 2, 38, AlignCenter, AlignBottom, line);

    // --- Ukuran tombol dibuat tetap supaya posisi tidak bergeser saat teks
    // berubah dari Start ke Stop atau saat nilai timer diedit. ----------------
    bool auto_on = model->auto_scan_enabled;
    bool auto_focus = (model->focus == DeauthAllFocusAuto);
    const char* auto_txt = "Auto Scan";
    uint16_t auto_tw = canvas_string_width(canvas, auto_txt);
    int auto_w = (int)auto_tw + BTN_PAD;

    bool start_focus = (model->focus == DeauthAllFocusStart);
    const char* start_txt = model->running ? "Stop" : "Start";
    uint16_t start_tw = canvas_string_width(canvas, "Start");
    uint16_t stop_tw = canvas_string_width(canvas, "Stop");
    if(stop_tw > start_tw) start_tw = stop_tw;
    int start_w = (int)start_tw + BTN_PAD;

    bool dur_focus = (model->focus == DeauthAllFocusDuration);
    char dur[16];
    if(model->editing_duration) {
        snprintf(dur, sizeof(dur), "< %us >", (unsigned)model->interval_s);
    } else {
        snprintf(dur, sizeof(dur), "Scan %us", (unsigned)model->interval_s);
    }
    uint16_t dur_tw = canvas_string_width(canvas, "Scan 60s");
    uint16_t edit_dur_tw = canvas_string_width(canvas, "< 60s >");
    if(edit_dur_tw > dur_tw) dur_tw = edit_dur_tw;
    int dur_w = (int)dur_tw + BTN_PAD;

    // Pusatkan ketiga kontrol sebagai satu grup. Jika font board sedikit lebih
    // lebar, kurangi jarak/padding agar bingkai tetap masuk layar.
    int button_gap = BTN_GAP;
    int button_padding = BTN_PAD;
    int controls_w = start_w + auto_w + dur_w + 2 * button_gap;
    const int max_controls_w = SCREEN_W - 2 * CONTROL_MARGIN;
    if(controls_w > max_controls_w) {
        button_gap = 2;
        button_padding = 4;
        start_w = (int)start_tw + button_padding;
        auto_w = (int)auto_tw + button_padding;
        dur_w = (int)dur_tw + button_padding;
        controls_w = start_w + auto_w + dur_w + 2 * button_gap;
    }
    if(controls_w > max_controls_w) {
        button_gap = 1;
        button_padding = 2;
        start_w = (int)start_tw + button_padding;
        auto_w = (int)auto_tw + button_padding;
        dur_w = (int)dur_tw + button_padding;
        controls_w = start_w + auto_w + dur_w + 2 * button_gap;
    }

    const int start_x = (SCREEN_W - controls_w) / 2;
    const int auto_x = start_x + start_w + button_gap;
    const int dur_x = auto_x + auto_w + button_gap;

    // Timer selalu memakai frame seperti tombol; saat diedit, frame terisi.
    draw_action_button(
        canvas, dur_x, CONTROL_ROW_Y, dur_w, BTN_H, dur, model->editing_duration, dur_focus);

    draw_action_button(
        canvas, start_x, CONTROL_ROW_Y, start_w, BTN_H, start_txt, false, start_focus);
    draw_action_button(
        canvas, auto_x, CONTROL_ROW_Y, auto_w, BTN_H, auto_txt, auto_on, auto_focus);
}

// ============================================================================
// INPUT CALLBACK
// ----------------------------------------------------------------------------
// OK/Up/Down/Left/Right → custom event (scene menafsirkan sesuai fokus & mode
// edit). Back dibiarkan (return false) supaya scene manager yang menangani:
// saat mode edit, scene meng-intercept Back untuk keluar dari edit; selain itu
// Back menutup scene seperti biasa.
// Catatan board: Up/Down dipakai T-Embed (encoder), Left/Right dipakai board
// lain (d-pad). Keduanya diterima sebagai aksi yang sama → benar di semua board.
// ============================================================================
static bool wlan_deauth_all_view_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* dispatcher = context;

    if(event->type != InputTypeShort && event->type != InputTypeLong) return false;

    uint32_t ev;
    switch(event->key) {
    case InputKeyOk:
        ev = WlanAppCustomEventDeauthAllOk;
        break;
    case InputKeyUp:
    case InputKeyRight:
        ev = WlanAppCustomEventDeauthAllNavInc;
        break;
    case InputKeyDown:
    case InputKeyLeft:
        ev = WlanAppCustomEventDeauthAllNavDec;
        break;
    default:
        return false; // Back dsb → scene manager
    }

    view_dispatcher_send_custom_event(dispatcher, ev);
    return true;
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
