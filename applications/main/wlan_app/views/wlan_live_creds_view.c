#include "wlan_live_creds_view.h"
#include "wlan_view_common.h"
#include <furi.h>
#include <gui/elements.h>
#include <input/input.h>
#include <string.h>
#include <stdio.h>

#define LC_VISIBLE_ROWS 4
#define LC_ROW_H 10
#define LC_ROW_BASE_Y 22 // Baseline der ersten Zeile

typedef struct {
    WlanCredEntry entries[WLAN_CRED_RING_SIZE];
    uint32_t count;
    uint32_t total;
    int sel;
    bool detail;
} WlanLiveCredsModel;

struct WlanLiveCredsView {
    View* view;
};

// ---------------------------------------------------------------------------
static void lc_ip_str(uint32_t ip_be, char* out, int sz) {
    const uint8_t* b = (const uint8_t*)&ip_be;
    snprintf(out, (size_t)sz, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static void lc_build_row(const WlanCredEntry* e, char* out, int sz) {
    char hostbuf[WLAN_CRED_STR_MAX];
    const char* host = e->host;
    if(!host[0]) {
        lc_ip_str(e->peer_ip, hostbuf, sizeof(hostbuf));
        host = hostbuf;
    }
    if(strcmp(e->proto, "DNS") == 0) {
        snprintf(out, (size_t)sz, "%s  %s", e->proto, host);
    } else if(strcmp(e->proto, "POST") == 0) {
        // POST: host + path kompakt; CT/Body sind im Detail.
        snprintf(out, (size_t)sz, "POST %s%s", host, e->user[0] ? e->user : "");
    } else if(e->user[0] && e->secret[0]) {
        snprintf(out, (size_t)sz, "%s %s  %s:%s", e->proto, host, e->user, e->secret);
    } else if(e->user[0]) {
        snprintf(out, (size_t)sz, "%s %s  %s", e->proto, host, e->user);
    } else if(e->secret[0]) {
        snprintf(out, (size_t)sz, "%s %s  :%s", e->proto, host, e->secret);
    } else {
        snprintf(out, (size_t)sz, "%s  %s", e->proto, host);
    }
}

static void lc_draw_field(Canvas* canvas, int x, int y, int max_w, const char* label, const char* val) {
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, x, y, label);
    int lw = (int)canvas_string_width(canvas, label);
    FuriString* s = furi_string_alloc_set(val && val[0] ? val : "-");
    elements_string_fit_width(canvas, s, (size_t)(max_w - lw - 3));
    canvas_draw_str(canvas, x + lw + 3, y, furi_string_get_cstr(s));
    furi_string_free(s);
}

// ---------------------------------------------------------------------------
static void lc_draw_list(Canvas* canvas, WlanLiveCredsModel* m) {
    wlan_view_draw_header(canvas, "Live Creds");

    // Lifetime-Count oben rechts.
    {
        canvas_set_font(canvas, FontSecondary);
        char cb[16];
        snprintf(cb, sizeof(cb), "%lu", (unsigned long)m->total);
        int cw = (int)canvas_string_width(canvas, cb);
        canvas_draw_str(canvas, 128 - 3 - cw, WLAN_VIEW_HEADER_BASELINE_Y, cb);
    }

    if(m->count == 0) {
        wlan_view_draw_empty_box(canvas, "No creds yet");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(
            canvas, 64, 52, AlignCenter, AlignBottom, "HTTP/FTP/POP3/SMTP/DNS");
        return;
    }

    int n = (int)m->count;
    if(m->sel < 0) m->sel = 0;
    if(m->sel >= n) m->sel = n - 1;
    int first = m->sel - LC_VISIBLE_ROWS / 2;
    int max_first = n - LC_VISIBLE_ROWS;
    if(max_first < 0) max_first = 0;
    if(first > max_first) first = max_first;
    if(first < 0) first = 0;

    canvas_set_font(canvas, FontSecondary);
    for(int row = 0; row < LC_VISIBLE_ROWS; row++) {
        int idx = first + row;
        if(idx >= n) break;
        int y = LC_ROW_BASE_Y + row * LC_ROW_H;
        char rowbuf[2 * WLAN_CRED_STR_MAX];
        lc_build_row(&m->entries[idx], rowbuf, sizeof(rowbuf));
        FuriString* s = furi_string_alloc_set(rowbuf);
        elements_string_fit_width(canvas, s, 124);
        if(idx == m->sel) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y - 8, 128, LC_ROW_H);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_str(canvas, 2, y, furi_string_get_cstr(s));
            canvas_set_color(canvas, ColorBlack);
        } else {
            canvas_draw_str(canvas, 2, y, furi_string_get_cstr(s));
        }
        furi_string_free(s);
    }

    elements_button_center(canvas, "Open");
}

static void lc_draw_detail(Canvas* canvas, WlanLiveCredsModel* m) {
    const WlanCredEntry* e =
        (m->sel >= 0 && (uint32_t)m->sel < m->count) ? &m->entries[m->sel] : NULL;
    char title[24];
    snprintf(title, sizeof(title), "%s detail", e ? e->proto : "?");
    wlan_view_draw_header(canvas, title);
    if(!e) {
        elements_button_left(canvas, "Back");
        return;
    }

    int y = 22;
    bool is_dns = (strcmp(e->proto, "DNS") == 0);
    bool has_raw = e->raw_body[0] != 0;

    if(is_dns) {
        lc_draw_field(canvas, 2, y, 124, "Query:", e->host);
        y += 10;
        lc_draw_field(canvas, 2, y, 124, "Type:", e->user[0] ? e->user : "A");
        y += 10;
    } else if(has_raw) {
        // Raw-Body-Modus (z.B. POST-Debug): Host, Path, dann Body mehrzeilig.
        if(e->host[0]) {
            lc_draw_field(canvas, 2, y, 124, "Host:", e->host);
            y += 9;
        }
        if(e->user[0]) {
            lc_draw_field(canvas, 2, y, 124, "Path:", e->user);
            y += 9;
        }
        canvas_set_font(canvas, FontSecondary);
        // Body: harter Umbruch nach 22 Zeichen, max 3 Zeilen.
#define LC_BODY_CHARS 22
#define LC_BODY_LINES 3
        int total = (int)strlen(e->raw_body);
        for(int line = 0; line < LC_BODY_LINES; line++) {
            int off = line * LC_BODY_CHARS;
            if(off >= total) break;
            int chunk = total - off;
            if(chunk > LC_BODY_CHARS) chunk = LC_BODY_CHARS;
            char tmp[LC_BODY_CHARS + 1];
            memcpy(tmp, e->raw_body + off, (size_t)chunk);
            tmp[chunk] = 0;
            canvas_draw_str(canvas, 2, y, tmp);
            y += 8;
        }
        if(total > LC_BODY_CHARS * LC_BODY_LINES) {
            canvas_draw_str(canvas, 2, 60, "... (CSV+serial)");
        }
#undef LC_BODY_CHARS
#undef LC_BODY_LINES
    } else {
        lc_draw_field(canvas, 2, y, 124, "User:", e->user);
        y += 10;
        lc_draw_field(canvas, 2, y, 124, "Pass:", e->secret);
        y += 10;
        if(e->host[0]) {
            lc_draw_field(canvas, 2, y, 124, "Host:", e->host);
            y += 10;
        }
        char peer[28];
        char ipb[16];
        lc_ip_str(e->peer_ip, ipb, sizeof(ipb));
        if(e->peer_port)
            snprintf(peer, sizeof(peer), "%s:%u", ipb, (unsigned)e->peer_port);
        else
            snprintf(peer, sizeof(peer), "%s", ipb);
        lc_draw_field(canvas, 2, y, 124, "Peer:", peer);
        y += 10;
        char vbuf[16];
        lc_ip_str(e->victim_ip, vbuf, sizeof(vbuf));
        uint32_t now = furi_get_tick();
        uint32_t ago = (now >= e->tick) ? (now - e->tick) / 1000 : 0;
        char line[48];
        snprintf(line, sizeof(line), "%s  (%lus ago)", vbuf, (unsigned long)ago);
        lc_draw_field(canvas, 2, y, 124, "From:", line);
    }

    elements_button_left(canvas, "Back");
}

static void lc_draw(Canvas* canvas, void* model) {
    WlanLiveCredsModel* m = model;
    canvas_clear(canvas);
    if(m->detail)
        lc_draw_detail(canvas, m);
    else
        lc_draw_list(canvas, m);
}

// ---------------------------------------------------------------------------
static bool lc_input(InputEvent* event, void* context) {
    WlanLiveCredsView* v = context;
    bool short_or_repeat = (event->type == InputTypeShort || event->type == InputTypeRepeat);
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    bool consumed = false;
    with_view_model(
        v->view,
        WlanLiveCredsModel * m,
        {
            int n = (int)m->count;
            switch(event->key) {
            case InputKeyUp:
                if(short_or_repeat && n > 0) {
                    if(m->sel > 0) m->sel--;
                    consumed = true;
                }
                break;
            case InputKeyDown:
                if(short_or_repeat && n > 0) {
                    if(m->sel < n - 1) m->sel++;
                    consumed = true;
                }
                break;
            case InputKeyOk:
                if(event->type == InputTypeShort) {
                    if(!m->detail) {
                        if(n > 0) m->detail = true;
                    } else {
                        m->detail = false;
                    }
                    consumed = true;
                }
                break;
            case InputKeyLeft:
                if(event->type == InputTypeShort && m->detail) {
                    m->detail = false;
                    consumed = true;
                }
                break;
            case InputKeyBack:
                if(event->type == InputTypeShort && m->detail) {
                    m->detail = false;
                    consumed = true;
                }
                // im Listen-Modus: nicht konsumieren → Navigation poppt die Scene
                break;
            default:
                break;
            }
        },
        consumed); // bei Änderung neu zeichnen

    return consumed;
}

// ---------------------------------------------------------------------------
WlanLiveCredsView* wlan_live_creds_view_alloc(void) {
    WlanLiveCredsView* v = malloc(sizeof(WlanLiveCredsView));
    v->view = view_alloc();
    view_set_context(v->view, v);
    view_allocate_model(v->view, ViewModelTypeLockFree, sizeof(WlanLiveCredsModel));
    view_set_draw_callback(v->view, lc_draw);
    view_set_input_callback(v->view, lc_input);

    WlanLiveCredsModel* m = view_get_model(v->view);
    memset(m, 0, sizeof(*m));
    view_commit_model(v->view, false);
    return v;
}

void wlan_live_creds_view_free(WlanLiveCredsView* v) {
    furi_assert(v);
    view_free(v->view);
    free(v);
}

View* wlan_live_creds_view_get_view(WlanLiveCredsView* v) {
    return v->view;
}

void wlan_live_creds_view_set_entries(
    WlanLiveCredsView* v, const WlanCredEntry* arr, uint32_t count, uint32_t total) {
    if(count > WLAN_CRED_RING_SIZE) count = WLAN_CRED_RING_SIZE;
    with_view_model(
        v->view,
        WlanLiveCredsModel * m,
        {
            if(count > 0 && arr) memcpy(m->entries, arr, count * sizeof(WlanCredEntry));
            m->count = count;
            m->total = total;
            if(m->sel >= (int)count) m->sel = count > 0 ? (int)count - 1 : 0;
        },
        true);
}

void wlan_live_creds_view_reset(WlanLiveCredsView* v) {
    with_view_model(
        v->view,
        WlanLiveCredsModel * m,
        {
            m->detail = false;
            m->sel = 0;
            m->count = 0;
            m->total = 0;
        },
        true);
}
