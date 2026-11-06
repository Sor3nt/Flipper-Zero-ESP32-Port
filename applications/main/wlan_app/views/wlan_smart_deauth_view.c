#include "wlan_smart_deauth_view.h"
#include "wlan_view_common.h"
#include "wlan_view_events.h"
#include "../wlan_smart_deauth.h"
#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/icon_animation.h>
#include <input/input.h>
#include <assets_icons.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// Layout bersama (semua mode konsisten)
//   Header line .... y=11  (wlan_view_draw_header)
//   Konten ......... y=13..50
//   Soft-button .... y=52..64 (elements_button_*)
// ============================================================================
#define SCREEN_W        128
#define LIST_TOP        13
#define ROW_H           9
#define LIST_ROWS       4
#define CH_COUNT        13

#define SSID_VIS_SCAN   11
#define SSID_VIS_ATK    20
#define SSID_VIS_MON    21

#define SCROLL_STEP_MS  150  // ms per langkah geser (dulu 40 → terlalu cepat)
#define SCROLL_DWELL_MS 1000 // jeda baca di awal tiap siklus (offset 0) → terasa mulus
#define SCROLL_GAP      3    // jumlah spasi antar pengulangan (harus sama dengan "   ")

static uint8_t  s_scroll_offset = 0;
static uint32_t s_scroll_last = 0;
static uint8_t  s_scroll_cursor_last = 0xFF;

// Animasi loading bawaan Flipper (A_Loading_24). Di-tie ke view saat alloc agar
// framenya menjadwalkan redraw sendiri; hanya digambar ketika sedang scanning.
static IconAnimation* s_loading = NULL;

// ============================================================================
// Helpers
// ============================================================================

// Scroll teks yang lebih panjang dari `visible`. Baris aktif (animate) memakai
// offset global yang berjalan; baris pasif hanya menampilkan potongan awal dan
// TIDAK menyentuh state global (fix: dulu baris pasif me-reset offset → macet).
static void scroll_string(const char* src, uint8_t visible, char* out, bool animate) {
    uint8_t slen = (uint8_t)strlen(src);

    if(slen == 0) {
        memset(out, ' ', visible);
        out[visible] = 0;
        return;
    }
    if(slen <= visible) {
        memcpy(out, src, slen);
        memset(out + slen, ' ', visible - slen);
        out[visible] = 0;
        return;
    }
    if(!animate) {
        memcpy(out, src, visible);
        out[visible] = 0;
        return;
    }

    uint8_t period = slen + SCROLL_GAP;
    uint32_t now = furi_get_tick();
    // Jeda lebih lama di offset 0 (awal teks) → mata sempat membaca, gulir mulus.
    uint32_t interval = (s_scroll_offset == 0) ? SCROLL_DWELL_MS : SCROLL_STEP_MS;
    if(now - s_scroll_last > interval) {
        s_scroll_offset++;
        if(s_scroll_offset >= period) s_scroll_offset = 0;
        s_scroll_last = now;
    }

    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s   %s", src, src); // SCROLL_GAP = 3 spasi
    uint8_t tmplen = (uint8_t)strlen(tmp);
    uint8_t off = s_scroll_offset;
    if((uint16_t)off + visible > tmplen) off = tmplen - visible;
    memcpy(out, &tmp[off], visible);
    out[visible] = 0;
}

// Ikon sinyal 3-bar (digambar, bukan glyph font) — ikut warna kanvas aktif
// sehingga aman pada baris terpilih yang di-invert.
static void draw_sig_bars(Canvas* canvas, int x, int baseline, int8_t rssi, bool has_ap) {
    uint8_t lvl = 0;
    if(has_ap) {
        if(rssi >= -55)      lvl = 3;
        else if(rssi >= -70) lvl = 2;
        else                 lvl = 1;
    }
    const uint8_t h[3] = {3, 5, 7};
    for(uint8_t i = 0; i < 3; i++) {
        int bx = x + i * 3; // lebar bar 2px + jarak 1px
        if(i < lvl) {
            canvas_draw_box(canvas, bx, baseline - h[i], 2, h[i]);
        } else {
            canvas_draw_box(canvas, bx, baseline - 1, 2, 1); // slot kosong: titik dasar
        }
    }
}

// Baris statistik seragam: label kiri + nilai kanan (dipakai attack & monitor).
static void draw_stat_row(Canvas* canvas, int y, const char* left, const char* right) {
    if(left && left[0]) canvas_draw_str(canvas, 2, y, left);
    if(right && right[0])
        canvas_draw_str_aligned(canvas, 126, y, AlignRight, AlignBottom, right);
}

// Badge status (pill) di kanan-atas area konten.
static void draw_status_pill(Canvas* canvas, const char* txt, bool filled) {
    canvas_set_font(canvas, FontSecondary);
    uint16_t w = canvas_string_width(canvas, txt);
    int bh = 11;
    int by = 13;
    int bw = (int)w + 8;
    int bx = 126 - bw;
    if(filled) {
        canvas_draw_rbox(canvas, bx, by, bw, bh, 2);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str_aligned(canvas, bx + bw / 2, by + bh / 2 + 1, AlignCenter, AlignCenter, txt);
        canvas_set_color(canvas, ColorBlack);
    } else {
        canvas_draw_rframe(canvas, bx, by, bw, bh, 2);
        canvas_draw_str_aligned(canvas, bx + bw / 2, by + bh / 2 + 1, AlignCenter, AlignCenter, txt);
    }
}

// ============================================================================
// SCAN MODE — daftar 13 kanal, 4 baris + scrollbar
// ============================================================================
static void draw_scan_mode(Canvas* canvas, SdViewModel* vm) {
    // Sedang scanning → tampilkan animasi loading bawaan Flipper, sembunyikan list.
    if(vm->scanning && s_loading) {
        uint8_t iw = icon_animation_get_width(s_loading);
        uint8_t ih = icon_animation_get_height(s_loading);
        int x = (SCREEN_W - iw) / 2;
        int y = LIST_TOP + ((LIST_ROWS * ROW_H) - ih) / 2;
        if(y < LIST_TOP) y = LIST_TOP;
        canvas_draw_icon_animation(canvas, x, y, s_loading);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, SCREEN_W / 2, 62, AlignCenter, AlignBottom, "Scanning...");
        return;
    }

    canvas_set_font(canvas, FontSecondary);

    uint8_t count = vm->detected_count;

    // Belum ada jaringan terdeteksi → pesan kosong + tombol rescan.
    if(count == 0) {
        canvas_draw_str_aligned(
            canvas, SCREEN_W / 2, LIST_TOP + (LIST_ROWS * ROW_H) / 2, AlignCenter, AlignCenter,
            "No networks found");
        elements_button_left(canvas, "Scan");
        return;
    }

    if(vm->scan_cursor != s_scroll_cursor_last) {
        s_scroll_offset = 0;
        s_scroll_last = furi_get_tick();
        s_scroll_cursor_last = vm->scan_cursor;
    }

    uint8_t first = 0;
    if(vm->scan_cursor >= LIST_ROWS) first = vm->scan_cursor - LIST_ROWS + 1;
    if(count > LIST_ROWS && first > count - LIST_ROWS) first = count - LIST_ROWS;

    for(uint8_t r = 0; r < LIST_ROWS; r++) {
        uint8_t pos = first + r;
        if(pos >= count) break;
        SdChannelView* ci = &vm->channels[vm->detected_map[pos]]; // hanya AP terdeteksi
        int top = LIST_TOP + r * ROW_H;
        int base = top + 7;
        bool sel = (pos == vm->scan_cursor);

        if(sel) {
            canvas_draw_box(canvas, 0, top, 124, ROW_H);
            canvas_set_color(canvas, ColorWhite);
        }

        char tag[4];
        snprintf(tag, sizeof(tag), "%u", ci->channel);
        canvas_draw_str(canvas, 2, base, tag);

        draw_sig_bars(canvas, 18, base, ci->rssi, true);

        char ssid_show[SSID_VIS_SCAN + 1];
        scroll_string(ci->ssid, SSID_VIS_SCAN, ssid_show, sel);
        canvas_draw_str(canvas, 29, base, ssid_show);

        char info[8];
        snprintf(info, sizeof(info), "%uc", ci->client_count);
        canvas_draw_str_aligned(canvas, 121, base, AlignRight, AlignBottom, info);

        if(sel) canvas_set_color(canvas, ColorBlack);
    }

    elements_scrollbar_pos(canvas, 125, LIST_TOP, LIST_ROWS * ROW_H, vm->scan_cursor, count);

    // "Scan" (pendek) di kiri + "Select" di tengah → ada jarak, tak bertabrakan.
    elements_button_left(canvas, "Scan");
    elements_button_center(canvas, "Select");
}

// ============================================================================
// ATTACK MODE
// ============================================================================
static void draw_attack_mode(Canvas* canvas, SdViewModel* vm) {
    char a[24], b[24];
    bool run = vm->attack_running;

    canvas_set_font(canvas, FontSecondary);

    // Baris 1: kanal (kiri) + status pill (kanan)
    snprintf(a, sizeof(a), "CH %u", vm->attack_channel);
    canvas_draw_str(canvas, 2, 22, a);
    draw_status_pill(canvas, run ? "LIVE" : "ARMED", run);

    // Baris 2: SSID (hero, scroll kalau panjang)
    char ssid_show[SSID_VIS_ATK + 1];
    scroll_string(vm->attack_ssid[0] ? vm->attack_ssid : "(hidden)", SSID_VIS_ATK, ssid_show, true);
    canvas_draw_str(canvas, 2, 32, ssid_show);

    // Baris stat 1
    snprintf(a, sizeof(a), "Frames %lu", (unsigned long)vm->attack_frames);
    snprintf(b, sizeof(b), "%lu/s", (unsigned long)vm->attack_fps);
    draw_stat_row(canvas, 41, a, b);

    // Baris stat 2
    snprintf(a, sizeof(a), "Cli %u", vm->attack_clients);
    if(run) {
        uint32_t age = vm->attack_beacon_age_ms;
        if(vm->attack_beacon_alive)
            snprintf(b, sizeof(b), "Bcn OK");
        else
            snprintf(b, sizeof(b), "Bcn %lus", (unsigned long)(age / 1000));
    } else {
        snprintf(b, sizeof(b), "ready");
    }
    draw_stat_row(canvas, 49, a, b);

    elements_button_left(canvas, "Back");
    elements_button_center(canvas, run ? "Stop" : "Run");
    elements_button_right(canvas, "Mon");
}

// ============================================================================
// MONITOR MODE — tata letak cermin dari attack (hero SSID + grid stat)
// ============================================================================
// Format throughput byte/detik → string ringkas presisi (1 desimal).
static void format_rate(uint32_t bps, char* out, size_t outlen) {
    if(bps >= 1048576u) {
        uint32_t whole = bps / 1048576u;
        uint32_t tenth = (uint32_t)(((uint64_t)(bps % 1048576u) * 10u) / 1048576u);
        snprintf(out, outlen, "%lu.%lu MB/s", (unsigned long)whole, (unsigned long)tenth);
    } else if(bps >= 1024u) {
        uint32_t whole = bps / 1024u;
        uint32_t tenth = (uint32_t)(((uint64_t)(bps % 1024u) * 10u) / 1024u);
        snprintf(out, outlen, "%lu.%lu KB/s", (unsigned long)whole, (unsigned long)tenth);
    } else {
        snprintf(out, outlen, "%lu B/s", (unsigned long)bps);
    }
}

static void draw_monitor_mode(Canvas* canvas, SdViewModel* vm) {
    char a[24], b[24];
    canvas_set_font(canvas, FontSecondary);

    // Baris 1 (y=21): SSID hero
    char ssid_show[SSID_VIS_MON + 1];
    scroll_string(vm->monitor_ssid[0] ? vm->monitor_ssid : "(hidden)", SSID_VIS_MON, ssid_show, true);
    canvas_draw_str(canvas, 2, 21, ssid_show);

    // Baris 2 (y=30): BSSID MAC lengkap (6 oktet)
    char mac[18];
    snprintf(
        mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", vm->monitor_bssid[0],
        vm->monitor_bssid[1], vm->monitor_bssid[2], vm->monitor_bssid[3], vm->monitor_bssid[4],
        vm->monitor_bssid[5]);
    canvas_draw_str(canvas, 2, 30, mac);

    // Baris 3 (y=39): kanal + RSSI
    snprintf(a, sizeof(a), "Ch %u", vm->monitor_channel);
    snprintf(b, sizeof(b), "RSSI %d", vm->monitor_rssi);
    draw_stat_row(canvas, 39, a, b);

    // Baris 4 (y=48): throughput presisi + jumlah client
    format_rate(vm->monitor_rate_bps, a, sizeof(a));
    snprintf(b, sizeof(b), "Cli %u", vm->monitor_client_count);
    draw_stat_row(canvas, 48, a, b);

    elements_button_left(canvas, "Attack");
}

// ============================================================================
// DRAW
// ============================================================================
static void draw_callback(Canvas* canvas, void* ctx) {
    SdViewModel* vm = ctx;
    canvas_clear(canvas);

    const char* title = "Smart Deauth";
    if(vm->mode == SdViewModeAttack)  title = "Deauth Attack";
    else if(vm->mode == SdViewModeMonitor) title = "Monitor";
    wlan_view_draw_header(canvas, title);

    switch(vm->mode) {
    case SdViewModeScan:    draw_scan_mode(canvas, vm); break;
    case SdViewModeAttack:  draw_attack_mode(canvas, vm); break;
    case SdViewModeMonitor: draw_monitor_mode(canvas, vm); break;
    }
}

// ============================================================================
// INPUT
// ============================================================================
static bool input_callback(InputEvent* event, void* ctx) {
    ViewDispatcher* d = ctx;
    if(event->type != InputTypeShort && event->type != InputTypeLong) return false;

    uint32_t ev = 0;
    if(event->key == InputKeyOk) ev = WlanAppCustomEventSmartDeauthOk;
    else if(event->key == InputKeyLeft) ev = WlanAppCustomEventSmartDeauthLeft;
    else if(event->key == InputKeyRight) ev = WlanAppCustomEventSmartDeauthRight;
    else if(event->key == InputKeyUp) ev = WlanAppCustomEventSmartDeauthUp;
    else if(event->key == InputKeyDown) ev = WlanAppCustomEventSmartDeauthDown;
    else return false;

    view_dispatcher_send_custom_event(d, ev);
    return true;
}

static void enter_callback(void* ctx) {
    UNUSED(ctx);
    // Reset state gulir agar teks tak tampil "sudah tergulir" saat view dibuka.
    s_scroll_offset = 0;
    s_scroll_last = furi_get_tick();
    s_scroll_cursor_last = 0xFF;
    if(s_loading) icon_animation_start(s_loading);
}

static void exit_callback(void* ctx) {
    UNUSED(ctx);
    if(s_loading) icon_animation_stop(s_loading);
}

View* wlan_smart_deauth_view_alloc(void) {
    View* v = view_alloc();
    view_allocate_model(v, ViewModelTypeLocking, sizeof(SdViewModel));
    view_set_draw_callback(v, draw_callback);
    view_set_input_callback(v, input_callback);
    view_set_enter_callback(v, enter_callback);
    view_set_exit_callback(v, exit_callback);

    s_loading = icon_animation_alloc(&A_Loading_24);
    view_tie_icon_animation(v, s_loading); // frame animasi menjadwalkan redraw view
    return v;
}

void wlan_smart_deauth_view_free(View* v) {
    if(s_loading) {
        icon_animation_stop(s_loading);
        icon_animation_free(s_loading);
        s_loading = NULL;
    }
    view_free(v);
}
