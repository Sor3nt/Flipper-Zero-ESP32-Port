#include "wlan_live_creds_view.h"
#include "wlan_view_common.h"
#include <furi.h>
#include <gui/elements.h>
#include <input/input.h>
#include <string.h>
#include <stdio.h>

// MiTM-Run-View — minimaler Log-Stil:
//   Header:   "MiTM" + rechts "N victims"
//   Zeilen:   "[INJ] host/path"   bzw.   "[CRED] user"
//   Kein Detail-Modus, nur Scroll.

#define LC_VISIBLE_ROWS 4
#define LC_ROW_H 10
#define LC_ROW_BASE_Y 22

typedef struct {
    WlanCredEntry entries[WLAN_CRED_RING_SIZE];
    uint32_t count;
    uint32_t total;
    uint16_t victims;
    int sel;
} WlanLiveCredsModel;

struct WlanLiveCredsView {
    View* view;
};

// ---------------------------------------------------------------------------
static void lc_build_row(const WlanCredEntry* e, char* out, int sz) {
    if(strcmp(e->proto, "INJ") == 0) {
        if(e->user[0]) {
            snprintf(out, (size_t)sz, "[INJ] %s%s", e->host, e->user);
        } else {
            snprintf(out, (size_t)sz, "[INJ] %s", e->host);
        }
    } else if(strcmp(e->proto, "LOG") == 0) {
        snprintf(out, (size_t)sz, "[LOG] %s", e->user[0] ? e->user : "(empty)");
    } else {
        // CRED-Eintrag: bevorzugt user, sonst secret, sonst host/proto.
        const char* who = e->user[0] ? e->user :
                         (e->secret[0] ? e->secret :
                         (e->host[0] ? e->host : e->proto));
        snprintf(out, (size_t)sz, "[CRED] %s", who);
    }
}

// ---------------------------------------------------------------------------
static void lc_draw(Canvas* canvas, void* model) {
    WlanLiveCredsModel* m = model;
    canvas_clear(canvas);
    wlan_view_draw_header(canvas, "MiTM");

    // Rechts: "N victims" (oder "1 victim").
    {
        canvas_set_font(canvas, FontSecondary);
        char cb[16];
        snprintf(cb, sizeof(cb),
            "%u %s", (unsigned)m->victims, m->victims == 1 ? "victim" : "victims");
        int cw = (int)canvas_string_width(canvas, cb);
        canvas_draw_str(canvas, 128 - 3 - cw, WLAN_VIEW_HEADER_BASELINE_Y, cb);
    }

    if(m->count == 0) {
        wlan_view_draw_empty_box(canvas, "No events yet");
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
            default:
                break;
            }
        },
        consumed);

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

void wlan_live_creds_view_set_victim_count(WlanLiveCredsView* v, uint16_t count) {
    with_view_model(v->view, WlanLiveCredsModel * m, { m->victims = count; }, true);
}

void wlan_live_creds_view_reset(WlanLiveCredsView* v) {
    with_view_model(
        v->view,
        WlanLiveCredsModel * m,
        {
            m->sel = 0;
            m->count = 0;
            m->total = 0;
            m->victims = 0;
        },
        true);
}
