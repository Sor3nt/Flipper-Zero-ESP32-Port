#include "evil_portal_captured_view.h"
#include <furi.h>
#include <gui/elements.h>
#include <assets_icons.h>
#include <input/input.h>
#include <string.h>

typedef struct {
    char ssid[33];
    char pwd[65];
} EvilPortalCapturedModel;

struct EvilPortalCapturedView {
    View* view;
    EvilPortalCapturedBackCb back_cb;
    void* back_ctx;
};

static void captured_draw(Canvas* canvas, void* model) {
    EvilPortalCapturedModel* m = model;
    canvas_clear(canvas);

    // ---- Header ----
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "Credentials Valid");
    canvas_draw_line(canvas, 0, 11, 127, 11);

    // ---- Hero icon (left) ----
    canvas_draw_icon(canvas, 2, 14, &I_Connected_62x31);

    // ---- Right text block ----
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 67, 19, "SSID:");
    {
        FuriString* s = furi_string_alloc_set(m->ssid[0] ? m->ssid : "-");
        elements_string_fit_width(canvas, s, 58);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 67, 28, furi_string_get_cstr(s));
        furi_string_free(s);
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 67, 36, "Pass:");
    {
        FuriString* s = furi_string_alloc_set(m->pwd[0] ? m->pwd : "-");
        elements_string_fit_width(canvas, s, 58);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 67, 45, furi_string_get_cstr(s));
        furi_string_free(s);
    }

    // ---- Saved hint ----
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 53, "Saved to /ext/wifi/evil_portal");

    // ---- Bottom button ----
    elements_button_left(canvas, "Back");
}

static bool captured_input(InputEvent* event, void* context) {
    EvilPortalCapturedView* v = context;
    if(event->type != InputTypeShort) return false;
    // T-Embed encoder: rotation without holding sends Up/Down. Treat both
    // rotation directions plus the regular Left/OK/Back as "leave this view".
    if(event->key == InputKeyBack || event->key == InputKeyLeft ||
       event->key == InputKeyUp || event->key == InputKeyDown ||
       event->key == InputKeyOk) {
        if(v->back_cb) v->back_cb(v->back_ctx);
        return true;
    }
    return false;
}

EvilPortalCapturedView* evil_portal_captured_view_alloc(void) {
    EvilPortalCapturedView* v = malloc(sizeof(EvilPortalCapturedView));
    v->view = view_alloc();
    v->back_cb = NULL;
    v->back_ctx = NULL;
    view_set_context(v->view, v);
    view_allocate_model(v->view, ViewModelTypeLockFree, sizeof(EvilPortalCapturedModel));
    view_set_draw_callback(v->view, captured_draw);
    view_set_input_callback(v->view, captured_input);

    EvilPortalCapturedModel* m = view_get_model(v->view);
    m->ssid[0] = 0;
    m->pwd[0] = 0;
    view_commit_model(v->view, false);

    return v;
}

void evil_portal_captured_view_free(EvilPortalCapturedView* v) {
    furi_assert(v);
    view_free(v->view);
    free(v);
}

View* evil_portal_captured_view_get_view(EvilPortalCapturedView* v) {
    return v->view;
}

void evil_portal_captured_view_set_data(
    EvilPortalCapturedView* v, const char* ssid, const char* pwd) {
    with_view_model(v->view, EvilPortalCapturedModel * m, {
        strncpy(m->ssid, ssid ? ssid : "", sizeof(m->ssid) - 1);
        m->ssid[sizeof(m->ssid) - 1] = 0;
        strncpy(m->pwd, pwd ? pwd : "", sizeof(m->pwd) - 1);
        m->pwd[sizeof(m->pwd) - 1] = 0;
    }, true);
}

void evil_portal_captured_view_set_back_callback(
    EvilPortalCapturedView* v, EvilPortalCapturedBackCb cb, void* ctx) {
    v->back_cb = cb;
    v->back_ctx = ctx;
}
