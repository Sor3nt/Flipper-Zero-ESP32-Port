#include "wlan_evil_portal_captured_view.h"
#include "wlan_view_common.h"
#include <furi.h>
#include <gui/elements.h>
#include <assets_icons.h>
#include <input/input.h>
#include <string.h>

typedef struct {
    char ssid[33];
    char pwd[65];
} WlanEvilPortalCapturedModel;

struct WlanEvilPortalCapturedView {
    View* view;
    WlanEvilPortalCapturedBackCb back_cb;
    void* back_ctx;
};

static void captured_draw(Canvas* canvas, void* model) {
    WlanEvilPortalCapturedModel* m = model;
    canvas_clear(canvas);

    wlan_view_draw_header(canvas, "Credentials Valid");

    canvas_draw_icon(canvas, 2, 14, &I_Connected_62x31);

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

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 53, "Saved to /ext/wifi/evil_portal");

    elements_button_left(canvas, "Back");
}

static bool captured_input(InputEvent* event, void* context) {
    WlanEvilPortalCapturedView* v = context;
    if(event->type != InputTypeShort) return false;
    if(event->key == InputKeyBack || event->key == InputKeyLeft ||
       event->key == InputKeyUp || event->key == InputKeyDown ||
       event->key == InputKeyOk) {
        if(v->back_cb) v->back_cb(v->back_ctx);
        return true;
    }
    return false;
}

WlanEvilPortalCapturedView* wlan_evil_portal_captured_view_alloc(void) {
    WlanEvilPortalCapturedView* v = malloc(sizeof(WlanEvilPortalCapturedView));
    v->view = view_alloc();
    v->back_cb = NULL;
    v->back_ctx = NULL;
    view_set_context(v->view, v);
    view_allocate_model(v->view, ViewModelTypeLockFree, sizeof(WlanEvilPortalCapturedModel));
    view_set_draw_callback(v->view, captured_draw);
    view_set_input_callback(v->view, captured_input);

    WlanEvilPortalCapturedModel* m = view_get_model(v->view);
    m->ssid[0] = 0;
    m->pwd[0] = 0;
    view_commit_model(v->view, false);

    return v;
}

void wlan_evil_portal_captured_view_free(WlanEvilPortalCapturedView* v) {
    furi_assert(v);
    view_free(v->view);
    free(v);
}

View* wlan_evil_portal_captured_view_get_view(WlanEvilPortalCapturedView* v) {
    return v->view;
}

void wlan_evil_portal_captured_view_set_data(
    WlanEvilPortalCapturedView* v, const char* ssid, const char* pwd) {
    with_view_model(v->view, WlanEvilPortalCapturedModel * m, {
        strncpy(m->ssid, ssid ? ssid : "", sizeof(m->ssid) - 1);
        m->ssid[sizeof(m->ssid) - 1] = 0;
        strncpy(m->pwd, pwd ? pwd : "", sizeof(m->pwd) - 1);
        m->pwd[sizeof(m->pwd) - 1] = 0;
    }, true);
}

void wlan_evil_portal_captured_view_set_back_callback(
    WlanEvilPortalCapturedView* v, WlanEvilPortalCapturedBackCb cb, void* ctx) {
    v->back_cb = cb;
    v->back_ctx = ctx;
}
