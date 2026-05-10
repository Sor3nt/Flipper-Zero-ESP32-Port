#include "wlan_evil_portal_view.h"
#include "wlan_view_common.h"
#include <furi.h>
#include <gui/elements.h>
#include <assets_icons.h>
#include <string.h>

#define HOURGLASS_PERIOD_MS 120

typedef struct {
    char ssid[33];
    char last_user[32];
    char busy_msg[24];
    uint32_t cred_count;
    uint16_t client_count;
    uint8_t channel;
    uint8_t hourglass_frame;
    bool busy;
    bool paused;
} WlanEvilPortalViewModel;

struct WlanEvilPortalView {
    View* view;
    FuriTimer* timer;
    WlanEvilPortalViewActionCb action_cb;
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

static void wlan_evil_portal_view_timer_cb(void* ctx) {
    WlanEvilPortalView* v = ctx;
    with_view_model(
        v->view,
        WlanEvilPortalViewModel * m,
        {
            if(m->busy) {
                m->hourglass_frame = (m->hourglass_frame + 1) % 7;
            }
        },
        true);
}

static void wlan_evil_portal_view_draw(Canvas* canvas, void* model) {
    WlanEvilPortalViewModel* m = model;
    canvas_clear(canvas);

    wlan_view_draw_header(canvas, "Evil Portal");

    // Channel rechts oben (FontSecondary, 3 px Abstand zum Rand).
    if(m->channel) {
        canvas_set_font(canvas, FontSecondary);
        char ch_buf[12];
        snprintf(ch_buf, sizeof(ch_buf), "Ch:%u", (unsigned)m->channel);
        uint16_t cw = canvas_string_width(canvas, ch_buf);
        canvas_draw_str(
            canvas, 128 - 3 - cw, WLAN_VIEW_HEADER_BASELINE_Y, ch_buf);
    }

    if(m->busy) {
        // Setup-Phase: Hourglass-Animation mittig + busy_msg.
        canvas_draw_icon(canvas, 64 - 12, 20, hourglass_icons[m->hourglass_frame % 7]);
        if(m->busy_msg[0]) {
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str_aligned(canvas, 64, 56, AlignCenter, AlignBottom, m->busy_msg);
        }
        return;
    }

    // Dolphin links.
    canvas_draw_icon(canvas, 0, 12, &I_WarningDolphin_45x42);

    // Rechte Spalte (Mitte ~x=87) — SSID dick, dann Clients/Creds.
    canvas_set_font(canvas, FontPrimary);
    {
        FuriString* s = furi_string_alloc_set(m->ssid[0] ? m->ssid : "-");
        elements_string_fit_width(canvas, s, 78);
        canvas_draw_str_aligned(
            canvas, 87, 24, AlignCenter, AlignBottom, furi_string_get_cstr(s));
        furi_string_free(s);
    }

    canvas_set_font(canvas, FontSecondary);
    char buf[24];
    snprintf(buf, sizeof(buf), "Clients: %u", (unsigned)m->client_count);
    canvas_draw_str_aligned(canvas, 87, 36, AlignCenter, AlignBottom, buf);

    snprintf(buf, sizeof(buf), "Creds: %lu", (unsigned long)m->cred_count);
    canvas_draw_str_aligned(canvas, 87, 46, AlignCenter, AlignBottom, buf);

    // Untere Trennlinie.
    canvas_draw_line(canvas, 0, 53, 128, 53);

    // Bolt-Icon + zuletzt erfasste Cred (links).
    canvas_draw_icon(canvas, 2, 54, &I_Charging_lightning_9x10);
    if(m->last_user[0]) {
        FuriString* s = furi_string_alloc_set(m->last_user);
        // Restbreite = 128 - (Bolt 9 + 4 px Spacer) - Soft-Button-Breite (~36).
        elements_string_fit_width(canvas, s, 76);
        canvas_draw_str(canvas, 14, 62, furi_string_get_cstr(s));
        furi_string_free(s);
    }

    // Start/Stop als rechter Soft-Button.
    elements_button_right(canvas, m->paused ? "Start" : "Stop");
}

static bool wlan_evil_portal_view_input(InputEvent* event, void* context) {
    WlanEvilPortalView* v = context;
    if(event->type != InputTypeShort) return false;
    // Encoder-Down löst den rechten Soft-Button (Start/Stop) aus
    // — analog zu wlan_deauther_view / wlan_sniffer_view.
    if(event->key == InputKeyDown) {
        if(v->action_cb) v->action_cb(v->action_ctx);
        return true;
    }
    return false;
}

WlanEvilPortalView* wlan_evil_portal_view_alloc(void) {
    WlanEvilPortalView* v = malloc(sizeof(WlanEvilPortalView));
    v->view = view_alloc();
    v->action_cb = NULL;
    v->action_ctx = NULL;
    view_set_context(v->view, v);
    view_allocate_model(v->view, ViewModelTypeLockFree, sizeof(WlanEvilPortalViewModel));
    view_set_draw_callback(v->view, wlan_evil_portal_view_draw);
    view_set_input_callback(v->view, wlan_evil_portal_view_input);

    WlanEvilPortalViewModel* m = view_get_model(v->view);
    m->ssid[0] = 0;
    m->last_user[0] = 0;
    m->busy_msg[0] = 0;
    m->cred_count = 0;
    m->client_count = 0;
    m->channel = 0;
    m->hourglass_frame = 0;
    m->busy = false;
    m->paused = false;
    view_commit_model(v->view, false);

    v->timer = furi_timer_alloc(wlan_evil_portal_view_timer_cb, FuriTimerTypePeriodic, v);
    furi_timer_start(v->timer, pdMS_TO_TICKS(HOURGLASS_PERIOD_MS));

    return v;
}

void wlan_evil_portal_view_free(WlanEvilPortalView* v) {
    furi_assert(v);
    furi_timer_stop(v->timer);
    furi_timer_free(v->timer);
    view_free(v->view);
    free(v);
}

View* wlan_evil_portal_view_get_view(WlanEvilPortalView* v) {
    return v->view;
}

void wlan_evil_portal_view_set_ssid(WlanEvilPortalView* v, const char* ssid) {
    with_view_model(v->view, WlanEvilPortalViewModel * m, {
        strncpy(m->ssid, ssid ? ssid : "", sizeof(m->ssid) - 1);
        m->ssid[sizeof(m->ssid) - 1] = 0;
    }, true);
}

void wlan_evil_portal_view_set_channel(WlanEvilPortalView* v, uint8_t channel) {
    with_view_model(v->view, WlanEvilPortalViewModel * m, { m->channel = channel; }, true);
}

void wlan_evil_portal_view_set_clients(WlanEvilPortalView* v, uint16_t count) {
    with_view_model(v->view, WlanEvilPortalViewModel * m, { m->client_count = count; }, true);
}

void wlan_evil_portal_view_set_creds(WlanEvilPortalView* v, uint32_t count) {
    with_view_model(v->view, WlanEvilPortalViewModel * m, { m->cred_count = count; }, true);
}

void wlan_evil_portal_view_set_last(WlanEvilPortalView* v, const char* user) {
    with_view_model(v->view, WlanEvilPortalViewModel * m, {
        strncpy(m->last_user, user ? user : "", sizeof(m->last_user) - 1);
        m->last_user[sizeof(m->last_user) - 1] = 0;
    }, true);
}

void wlan_evil_portal_view_set_busy(WlanEvilPortalView* v, bool busy, const char* msg) {
    with_view_model(v->view, WlanEvilPortalViewModel * m, {
        m->busy = busy;
        if(busy) {
            strncpy(m->busy_msg, msg ? msg : "", sizeof(m->busy_msg) - 1);
            m->busy_msg[sizeof(m->busy_msg) - 1] = 0;
        } else {
            m->busy_msg[0] = 0;
        }
    }, true);
}

void wlan_evil_portal_view_set_paused(WlanEvilPortalView* v, bool paused) {
    with_view_model(v->view, WlanEvilPortalViewModel * m, { m->paused = paused; }, true);
}

void wlan_evil_portal_view_set_action_callback(
    WlanEvilPortalView* v, WlanEvilPortalViewActionCb cb, void* ctx) {
    v->action_cb = cb;
    v->action_ctx = ctx;
}
