#include "ble_wardriving_view.h"
#include "../ble_spam_app.h"
#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// Layout
//   Header line ..... y=11
//   List rows ....... 2 entries × 20px (name y=20/40, MAC y=28/48)
//   Detail content .. 5 lines × 8px (y=18,26,34,42,50)
//   Footer .......... elements_button_left/right y=54..64
//   Center info ..... baseline y=61
// ============================================================================
#define SCREEN_W        128
#define LIST_TOP        13
#define LIST_BOTTOM     52
#define ROW0_Y          20
#define ROW_SPACING     20
#define VISIBLE_ROWS    2
#define FOOTER_MID_Y    61
#define NAME_X          4
#define INFO_RIGHT_PAD  4
#define BOX_TOP_PAD     8
#define BOX_HEIGHT      20
#define NAME_VIS        12

#define SCROLL_STEP_MS  150
#define SCROLL_DWELL_MS 1000

static uint8_t  s_scroll_offset = 0;
static uint32_t s_scroll_last = 0;
static uint16_t s_scroll_last_cursor = 0xFFFF;

// ============================================================================
// Helpers
// ============================================================================
static const char* addr_type_str(BleAddrType t) {
    switch(t) {
    case BleAddrPublic: return "PUB";
    case BleAddrRandom: return "RND";
    default:            return "???";
    }
}

static void draw_header(Canvas* canvas, const char* title) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, SCREEN_W / 2, 10, AlignCenter, AlignBottom, title);
    canvas_draw_line(canvas, 0, 11, SCREEN_W, 11);
}

static void draw_empty_box(Canvas* canvas, const char* text) {
    canvas_set_font(canvas, FontSecondary);
    uint16_t tw = canvas_string_width(canvas, text);
    uint8_t bw = (uint8_t)(tw + 12);
    if(bw > SCREEN_W - 4) bw = SCREEN_W - 4;
    uint8_t bh = 16;
    uint8_t bx = (uint8_t)((SCREEN_W - bw) / 2);
    uint8_t by = (uint8_t)((13 + 52 - bh) / 2);
    elements_slightly_rounded_frame(canvas, bx, by, bw, bh);
    canvas_draw_str_aligned(canvas, SCREEN_W / 2, by + bh / 2 + 1,
                             AlignCenter, AlignCenter, text);
}

static void scroll_name(const char* src, uint8_t visible, char* out, bool selected) {
    uint8_t slen = (uint8_t)strlen(src);
    if(slen == 0) { memset(out, ' ', visible); out[visible] = 0; return; }
    if(slen <= visible || !selected) {
        uint8_t n = slen < visible ? slen : visible;
        memcpy(out, src, n);
        memset(out + n, ' ', visible - n);
        out[visible] = 0;
        return;
    }
    uint8_t period = slen + 3;
    uint32_t now = furi_get_tick();
    uint32_t interval = (s_scroll_offset == 0) ? SCROLL_DWELL_MS : SCROLL_STEP_MS;
    if(now - s_scroll_last > interval) {
        s_scroll_offset++;
        if(s_scroll_offset >= period) s_scroll_offset = 0;
        s_scroll_last = now;
    }
    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s   %s", src, src);
    uint8_t tmplen = (uint8_t)strlen(tmp);
    uint8_t off = s_scroll_offset;
    if((uint16_t)off + visible > tmplen) off = tmplen - visible;
    memcpy(out, &tmp[off], visible);
    out[visible] = 0;
}

// ============================================================================
// LIST VIEW
// ============================================================================
static void draw_list(Canvas* canvas, BleWardrivingModel* m) {
    canvas_set_font(canvas, FontSecondary);

    if(m->count == 0) {
        draw_empty_box(canvas, m->running ? "Scanning BLE..." : "No devices");
        elements_button_left(canvas, "Rescan");
        elements_button_right(canvas, "Info");
        return;
    }

    if(m->cursor != s_scroll_last_cursor) {
        s_scroll_offset = 0;
        s_scroll_last = furi_get_tick();
        s_scroll_last_cursor = m->cursor;
    }

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
        BleDeviceView* d = &m->devices[idx];
        int y_name = ROW0_Y + r * ROW_SPACING;
        int y_mac  = y_name + 8;
        bool sel = (idx == m->cursor);

        // Selection box dengan padding 2px atas-bawah
        if(sel) {
            canvas_draw_box(canvas, 0, y_name - BOX_TOP_PAD, row_w, BOX_HEIGHT);
            canvas_set_color(canvas, ColorWhite);
        }

        // Line 1: Name (left) + rssi/addr_type (right)
        char name_show[NAME_VIS + 1];
        scroll_name(d->has_name ? d->name : "(no name)",
                    NAME_VIS, name_show, sel);
        canvas_draw_str(canvas, NAME_X, y_name, name_show);

        char info[16];
        snprintf(info, sizeof(info), "%d %s",
                 d->rssi, addr_type_str(d->addr_type));
        canvas_draw_str_aligned(canvas, row_w - INFO_RIGHT_PAD, y_name,
                                AlignRight, AlignBottom, info);

        // Line 2: MAC lengkap
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 d->mac[0], d->mac[1], d->mac[2],
                 d->mac[3], d->mac[4], d->mac[5]);
        canvas_draw_str(canvas, NAME_X, y_mac, mac_str);

        if(sel) canvas_set_color(canvas, ColorBlack);
    }

    if(has_bar) {
        elements_scrollbar_pos(canvas, 125, LIST_TOP, LIST_BOTTOM - LIST_TOP,
                               m->cursor, m->count);
    }

    // Footer: Flipper-style buttons
    elements_button_left(canvas, "Rescan");
    elements_button_right(canvas, "Info");

    // Center: total count
    char mid[8];
    snprintf(mid, sizeof(mid), "%u", m->total);
    canvas_draw_str_aligned(canvas, 64, FOOTER_MID_Y,
                            AlignCenter, AlignBottom, mid);
}

// ============================================================================
// DETAIL VIEW — FIX: 5 lines × 8px spacing
// ============================================================================
static void draw_detail(Canvas* canvas, BleWardrivingModel* m) {
    canvas_set_font(canvas, FontSecondary);

    if(!m->detail_valid) {
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignBottom,
                                 "(device gone)");
        elements_button_left(canvas, "Back");
        return;
    }

    BleDeviceEntry* d = &m->detail;
    char line[48];

    // ============================================================
    // Line 1 (y=18): Name + addr type
    // ============================================================
    char name_big[22];
    const char* src = d->has_name ? d->name : "(no name)";
    uint8_t slen = (uint8_t)strlen(src);
    if(slen <= 15) {
        memcpy(name_big, src, slen);
        name_big[slen] = 0;
    } else {
        static uint32_t s_det_last = 0;
        static uint8_t  s_det_off = 0;
        uint32_t now = furi_get_tick();
        if(now - s_det_last > SCROLL_STEP_MS) {
            s_det_off++;
            if(s_det_off >= slen + 3) s_det_off = 0;
            s_det_last = now;
        }
        char tmp[80];
        snprintf(tmp, sizeof(tmp), "%s   %s", src, src);
        uint8_t tmplen = (uint8_t)strlen(tmp);
        uint8_t off = s_det_off;
        if((uint16_t)off + 15 > tmplen) off = tmplen - 15;
        memcpy(name_big, &tmp[off], 15);
        name_big[15] = 0;
    }
    canvas_draw_str(canvas, 4, 18, name_big);
    canvas_draw_str_aligned(canvas, 124, 18, AlignRight, AlignBottom,
                             addr_type_str(d->addr_type));

    // ============================================================
    // Line 2 (y=26): MAC lengkap
    // ============================================================
    snprintf(line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X",
             d->mac[0], d->mac[1], d->mac[2],
             d->mac[3], d->mac[4], d->mac[5]);
    canvas_draw_str(canvas, 4, 26, line);

    // ============================================================
    // Line 3 (y=34): Vendor
    // ============================================================
    snprintf(line, sizeof(line), "%.20s",
             d->vendor[0] ? d->vendor : "Unknown");
    canvas_draw_str(canvas, 4, 34, line);

    // ============================================================
    // Line 4 (y=42): RSSI (current min~max)
    // ============================================================
    snprintf(line, sizeof(line), "RSSI %d (%d~%d)",
             d->rssi, d->rssi_min, d->rssi_max);
    canvas_draw_str(canvas, 4, 42, line);

    // ============================================================
    // Line 5 (y=50): Mfr + Adv
    // ============================================================
    if(d->has_mfr_data) {
        snprintf(line, sizeof(line), "Mfr:0x%04X Adv:%lu",
                 d->company_id, (unsigned long)d->adv_count);
    } else {
        snprintf(line, sizeof(line), "Mfr:---- Adv:%lu",
                 (unsigned long)d->adv_count);
    }
    canvas_draw_str(canvas, 4, 50, line);

    // ============================================================
    // Footer: Flipper-style button
    // ============================================================
    elements_button_left(canvas, "Back");

    // Right: index
    char idx_str[12];
    snprintf(idx_str, sizeof(idx_str), "%u/%u",
             (unsigned)(m->cursor + 1), (unsigned)m->count);
    canvas_draw_str_aligned(canvas, 126, FOOTER_MID_Y,
                            AlignRight, AlignBottom, idx_str);
}

// ============================================================================
// Draw
// ============================================================================
static void draw_cb(Canvas* canvas, void* ctx) {
    BleWardrivingModel* m = ctx;
    canvas_clear(canvas);
    draw_header(canvas, "BLE Wardriving");
    if(m->exit_pending) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, SCREEN_W / 2, 34,
                                 AlignCenter, AlignCenter, "Stopping BLE...");
    } else if(m->detail_mode) {
        draw_detail(canvas, m);
    } else {
        draw_list(canvas, m);
    }
}

// ============================================================================
// Input
// ============================================================================
static bool input_cb(InputEvent* event, void* ctx) {
    ViewDispatcher* d = ctx;
    if(event->type != InputTypeShort && event->type != InputTypeLong) return false;

    uint32_t ev;
    switch(event->key) {
    case InputKeyOk:    ev = BleSpamWardrivingOk;    break;
    case InputKeyUp:    ev = BleSpamWardrivingUp;    break;
    case InputKeyDown:  ev = BleSpamWardrivingDown;  break;
    case InputKeyLeft:  ev = BleSpamWardrivingLeft;  break;
    case InputKeyRight: ev = BleSpamWardrivingRight; break;
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

View* ble_wardriving_view_alloc(void) {
    View* v = view_alloc();
    view_allocate_model(v, ViewModelTypeLocking, sizeof(BleWardrivingModel));
    view_set_draw_callback(v, draw_cb);
    view_set_input_callback(v, input_cb);
    view_set_enter_callback(v, enter_cb);
    return v;
}

void ble_wardriving_view_free(View* v) {
    view_free(v);
}
