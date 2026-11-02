#include "wlan_live_scan_view.h"
#include "wlan_view_common.h"
#include "wlan_view_events.h"
#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// Layout
//   Header:          y=0..11
//   List rows:       y=13..48 (2 entries, box 18px each)
//   Footer buttons:  y=49..63 (elements_button_left/right)
//   Center info:     y=61 (antara kedua tombol)
// ============================================================================
#define SCREEN_W        128
#define LIST_TOP        13
#define ROW0_Y          21
#define ROW_SPACING     18
#define VISIBLE_ROWS    2
#define LIST_BOTTOM     48
#define FOOTER_MID_Y    61
#define SSID_X          4
#define INFO_RIGHT_PAD  4
#define ROW_BOX_TOP     8
#define ROW_BOX_HEIGHT  18

#define SCROLL_STEP_MS  150
#define SCROLL_DWELL_MS 1000

static uint8_t  s_scroll_offset = 0;
static uint32_t s_scroll_last = 0;
static uint16_t s_scroll_last_cursor = 0xFFFF;

// ============================================================================
// Helpers
// ============================================================================
static const char* auth_short(const LiveApEntry* ap) {
    if(ap->has_wpa3)       return "W3";
    if(ap->has_rsn)        return "W2";
    if(ap->authmode != 0)  return "WP";
    return "OP";
}

static void scroll_ssid(Canvas* canvas, const char* src, uint16_t max_width,
                        char* out, bool selected) {
    uint8_t slen = (uint8_t)strlen(src);
    if(slen == 0) { out[0] = 0; return; }
    bool should_scroll = selected && canvas_string_width(canvas, src) > max_width;
    uint8_t offset = 0;
    char tmp[72];
    const char* text = src;
    size_t text_len = slen;

    if(should_scroll) {
        uint8_t period = slen + 3;
        snprintf(tmp, sizeof(tmp), "%s   %s", src, src);
        text = tmp;
        text_len = strlen(tmp);
        uint32_t now = furi_get_tick();
        uint32_t interval = (s_scroll_offset == 0) ? SCROLL_DWELL_MS : SCROLL_STEP_MS;
        if(now - s_scroll_last > interval) {
            s_scroll_offset++;
            if(s_scroll_offset >= period) s_scroll_offset = 0;
            s_scroll_last = now;
        }
        offset = s_scroll_offset;
        if(offset >= text_len) offset = 0;
    }

    out[0] = 0;
    size_t out_len = 0;
    for(size_t i = offset; i < text_len && out_len < 70; i++) {
        out[out_len] = text[i];
        out[out_len + 1] = 0;
        if(canvas_string_width(canvas, out) > max_width) {
            out[out_len] = 0;
            break;
        }
        out_len++;
    }
}

static void format_mac(const uint8_t* mac, char* out, size_t out_size) {
    snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ============================================================================
// LIST VIEW
// ============================================================================
static void draw_list(Canvas* canvas, WlanLiveScanModel* m) {
    canvas_set_font(canvas, FontSecondary);

    // Empty state
    if(m->count == 0) {
        wlan_view_draw_empty_box(canvas, m->running ? "Scanning..." : "No networks");
        elements_button_left(canvas, "Rescan");
        elements_button_right(canvas, "Info");
        return;
    }

    // Reset scroll kalau cursor pindah
    if(m->cursor != s_scroll_last_cursor) {
        s_scroll_offset = 0;
        s_scroll_last = furi_get_tick();
        s_scroll_last_cursor = m->cursor;
    }

    // Compute window
    uint16_t first = 0;
    if(m->cursor >= VISIBLE_ROWS) first = m->cursor - VISIBLE_ROWS + 1;
    if(m->count > VISIBLE_ROWS && first + VISIBLE_ROWS > m->count) {
        first = m->count - VISIBLE_ROWS;
    }

    bool has_bar = (m->count > VISIBLE_ROWS);
    int row_w = has_bar ? 124 : SCREEN_W;

    for(uint8_t r = 0; r < VISIBLE_ROWS; r++) {
        uint16_t idx = first + r;
        if(idx >= m->count) break;

        LiveApEntry* ap = &m->aps[idx];
        int y_ssid = ROW0_Y + r * ROW_SPACING;
        int y_mac  = y_ssid + 8;
        bool sel = (idx == m->cursor);

        // Selection box: DENGAN padding
        if(sel) {
            canvas_draw_box(canvas, 0, y_ssid - ROW_BOX_TOP, row_w, ROW_BOX_HEIGHT);
            canvas_set_color(canvas, ColorWhite);
        }

        // Line 1: SSID (left) + channel/rssi/auth (right)
        char info[20];
        snprintf(info, sizeof(info), "%u %d %s",
                 ap->channel, ap->rssi, auth_short(ap));
        int info_x = row_w - INFO_RIGHT_PAD - canvas_string_width(canvas, info);
        int ssid_width = info_x - SSID_X - 2;
        if(ssid_width < 0) ssid_width = 0;

        char ssid_show[72];
        scroll_ssid(canvas, ap->ssid[0] ? ap->ssid : "(hidden)",
                    (uint16_t)ssid_width, ssid_show, sel);
        canvas_draw_str(canvas, SSID_X, y_ssid, ssid_show);
        canvas_draw_str_aligned(canvas, row_w - INFO_RIGHT_PAD, y_ssid,
                                AlignRight, AlignBottom, info);

        // Line 2: MAC lengkap
        char mac_str[18];
        format_mac(ap->bssid, mac_str, sizeof(mac_str));
        canvas_draw_str(canvas, SSID_X, y_mac, mac_str);

        if(sel) canvas_set_color(canvas, ColorBlack);
    }

    if(has_bar) {
        elements_scrollbar_pos(canvas, 125, LIST_TOP, LIST_BOTTOM - LIST_TOP,
                               m->cursor, m->count);
    }

    // Footer: Flipper-style buttons
    elements_button_left(canvas, "Rescan");
    elements_button_right(canvas, "Info");

    // Center info between buttons
    canvas_set_font(canvas, FontSecondary);
    char mid[16];
    snprintf(mid, sizeof(mid), "%s %u",
             m->hopping ? "HOP" : "LCK", m->current_channel);
    canvas_draw_str_aligned(canvas, 64, FOOTER_MID_Y, AlignCenter, AlignBottom, mid);
}

// ============================================================================
// DETAIL VIEW
// ============================================================================
static void draw_detail(Canvas* canvas, WlanLiveScanModel* m) {
    if(m->cursor >= m->count) return;
    LiveApEntry* ap = &m->aps[m->cursor];

    canvas_set_font(canvas, FontSecondary);

    // Line 1: SSID (y=19)
    char ssid_big[24];
    const char* src = ap->ssid[0] ? ap->ssid : "(hidden)";
    uint8_t slen = (uint8_t)strlen(src);
    if(slen <= 21) {
        memcpy(ssid_big, src, slen);
        ssid_big[slen] = 0;
    } else {
        static uint32_t s_det_last = 0;
        static uint8_t  s_det_off = 0;
        uint32_t now = furi_get_tick();
        if(now - s_det_last > 200) {
            s_det_off++;
            if(s_det_off >= slen + 3) s_det_off = 0;
            s_det_last = now;
        }
        char tmp[80];
        snprintf(tmp, sizeof(tmp), "%s   %s", src, src);
        uint8_t tmplen = (uint8_t)strlen(tmp);
        uint8_t off = s_det_off;
        if((uint16_t)off + 21 > tmplen) off = tmplen - 21;
        memcpy(ssid_big, &tmp[off], 21);
        ssid_big[21] = 0;
    }
    canvas_draw_str(canvas, 4, 19, ssid_big);

    // Line 2: MAC lengkap (y=28)
    char mac_str[18];
    format_mac(ap->bssid, mac_str, sizeof(mac_str));
    canvas_draw_str(canvas, 4, 28, mac_str);

    // Line 3: Vendor + Channel + RSSI (y=37)
    char line[48];
    char vendor_short[16];
    snprintf(vendor_short, sizeof(vendor_short), "%.12s",
             ap->vendor[0] ? ap->vendor : "Unknown");
    snprintf(line, sizeof(line), "%s CH%u %ddBm",
             vendor_short, ap->channel, ap->rssi);
    canvas_draw_str(canvas, 4, 37, line);

    // Line 4: Auth + PMF + Beacon (y=46)
    const char* auth = ap->has_wpa3 ? "WPA3" :
                       (ap->has_rsn ? "WPA2" :
                       (ap->authmode != 0 ? "WEP" : "OPEN"));
    snprintf(line, sizeof(line), "%-4s %s Bcn:%lu",
             auth,
             ap->has_pmf ? "PMF" : "   ",
             (unsigned long)ap->beacon_count);
    canvas_draw_str(canvas, 4, 46, line);

    // Footer: Flipper buttons
    elements_button_left(canvas, "Back");
    // Right: show index instead of button
    char idx_str[12];
    snprintf(idx_str, sizeof(idx_str), "%u/%u",
             (unsigned)(m->cursor + 1), (unsigned)m->count);
    canvas_draw_str_aligned(canvas, 126, FOOTER_MID_Y,
                            AlignRight, AlignBottom, idx_str);
}

// ============================================================================
// Draw / Input
// ============================================================================
static void draw_cb(Canvas* canvas, void* ctx) {
    WlanLiveScanModel* m = ctx;
    canvas_clear(canvas);
    wlan_view_draw_header(canvas, "WiFi Live Scan");

    if(m->detail_mode) {
        draw_detail(canvas, m);
    } else {
        draw_list(canvas, m);
    }
}

static bool input_cb(InputEvent* event, void* ctx) {
    ViewDispatcher* d = ctx;
    if(event->type != InputTypeShort && event->type != InputTypeLong) return false;

    uint32_t ev = 0;
    switch(event->key) {
    case InputKeyOk:
        if(event->type != InputTypeShort) return false;
        ev = WlanAppCustomEventLiveScanOk;
        break;
    case InputKeyRight:
        // FIX U3: Right short = enter/exit detail (sama dengan OK)
        // Right long  = toggle channel hopping
        if(event->type == InputTypeShort) {
            ev = WlanAppCustomEventLiveScanOk;
        } else {
            ev = WlanAppCustomEventLiveScanRight;
        }
        break;
    case InputKeyUp:
        if(event->type != InputTypeShort) return false;
        ev = WlanAppCustomEventLiveScanUp;
        break;
    case InputKeyDown:
        if(event->type != InputTypeShort) return false;
        ev = WlanAppCustomEventLiveScanDown;
        break;
    case InputKeyLeft:
        if(event->type != InputTypeShort) return false;
        ev = WlanAppCustomEventLiveScanLeft;
        break;
    default: return false;
    }

    view_dispatcher_send_custom_event(d, ev);
    return true;
}

static void enter_cb(void* ctx) {
    UNUSED(ctx);
    s_scroll_offset = 0;
    s_scroll_last = furi_get_tick();
    s_scroll_last_cursor = 0xFFFF;
}

View* wlan_live_scan_view_alloc(void) {
    View* v = view_alloc();
    view_allocate_model(v, ViewModelTypeLocking, sizeof(WlanLiveScanModel));
    view_set_draw_callback(v, draw_cb);
    view_set_input_callback(v, input_cb);
    view_set_enter_callback(v, enter_cb);
    return v;
}

void wlan_live_scan_view_free(View* v) {
    view_free(v);
}
