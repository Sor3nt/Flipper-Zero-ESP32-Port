#include "evil_portal_view.h"
#include <furi.h>
#include <gui/elements.h>
#include <string.h>

typedef struct {
    char ssid[33];
    char last_user[32];
    char status[16];
    uint32_t cred_count;
    uint16_t client_count;
    uint8_t channel;
} EvilPortalViewModel;

struct EvilPortalView {
    View* view;
};

static void evil_portal_view_draw_callback(Canvas* canvas, void* model) {
    EvilPortalViewModel* m = model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 9, AlignCenter, AlignTop, "Evil Portal");

    canvas_set_font(canvas, FontSecondary);

    char line[40];
    snprintf(line, sizeof(line), "SSID:%s", m->ssid[0] ? m->ssid : "-");
    canvas_draw_str(canvas, 2, 24, line);

    snprintf(line, sizeof(line), "Ch:%u Cli:%u", m->channel, m->client_count);
    canvas_draw_str(canvas, 2, 34, line);

    snprintf(line, sizeof(line), "Creds:%lu", (unsigned long)m->cred_count);
    canvas_draw_str(canvas, 2, 44, line);

    if(m->last_user[0]) {
        snprintf(line, sizeof(line), "%s", m->last_user);
        canvas_draw_str(canvas, 2, 54, line);
    } else if(m->status[0]) {
        canvas_draw_str(canvas, 2, 54, m->status);
    }

    elements_button_center(canvas, "Stop");
}

static bool evil_portal_view_input_callback(InputEvent* event, void* context) {
    UNUSED(context);
    if(event->type == InputTypePress && event->key == InputKeyOk) {
        return true;
    }
    return false;
}

EvilPortalView* evil_portal_view_alloc(void) {
    EvilPortalView* v = malloc(sizeof(EvilPortalView));
    v->view = view_alloc();
    view_allocate_model(v->view, ViewModelTypeLockFree, sizeof(EvilPortalViewModel));
    view_set_draw_callback(v->view, evil_portal_view_draw_callback);
    view_set_input_callback(v->view, evil_portal_view_input_callback);

    EvilPortalViewModel* m = view_get_model(v->view);
    m->ssid[0] = 0;
    m->last_user[0] = 0;
    strcpy(m->status, "Starting...");
    m->cred_count = 0;
    m->client_count = 0;
    m->channel = 0;
    view_commit_model(v->view, false);

    return v;
}

void evil_portal_view_free(EvilPortalView* v) {
    furi_assert(v);
    view_free(v->view);
    free(v);
}

View* evil_portal_view_get_view(EvilPortalView* v) {
    return v->view;
}

void evil_portal_view_set_ssid(EvilPortalView* v, const char* ssid) {
    with_view_model(v->view, EvilPortalViewModel * m, {
        strncpy(m->ssid, ssid ? ssid : "", sizeof(m->ssid) - 1);
        m->ssid[sizeof(m->ssid) - 1] = 0;
    }, true);
}

void evil_portal_view_set_channel(EvilPortalView* v, uint8_t channel) {
    with_view_model(v->view, EvilPortalViewModel * m, {
        m->channel = channel;
    }, true);
}

void evil_portal_view_set_clients(EvilPortalView* v, uint16_t count) {
    with_view_model(v->view, EvilPortalViewModel * m, {
        m->client_count = count;
    }, true);
}

void evil_portal_view_set_creds(EvilPortalView* v, uint32_t count) {
    with_view_model(v->view, EvilPortalViewModel * m, {
        m->cred_count = count;
    }, true);
}

void evil_portal_view_set_last(EvilPortalView* v, const char* user) {
    with_view_model(v->view, EvilPortalViewModel * m, {
        strncpy(m->last_user, user ? user : "", sizeof(m->last_user) - 1);
        m->last_user[sizeof(m->last_user) - 1] = 0;
    }, true);
}

void evil_portal_view_set_status(EvilPortalView* v, const char* status) {
    with_view_model(v->view, EvilPortalViewModel * m, {
        strncpy(m->status, status ? status : "", sizeof(m->status) - 1);
        m->status[sizeof(m->status) - 1] = 0;
    }, true);
}
