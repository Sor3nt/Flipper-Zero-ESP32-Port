#include "wlan_webfs.h"
#include <wlan_hal.h>

#include <furi.h>
#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>
#include <btshim.h>

#include <string.h>
#include <stdio.h>

#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_http_server.h>

#define TAG "WlanWebFs"

#define WEBFS_DIR        "/ext/webfs"
#define WEBFS_CONFIG     "/ext/webfs/config.txt"
#define WEBFS_INDEX_PATH "/ext/webfs/index.html"
#define WEBFS_IO_CHUNK   4096

/* ─────────────────────── state ─────────────────────── */

static httpd_handle_t s_http = NULL;
static esp_netif_t* s_ap_netif = NULL;
static bool s_running = false;
static bool s_is_ap = false;
static bool s_evt_registered = false;
static bool s_bt_was_on = false;
static char s_ip[16] = "0.0.0.0";
static volatile int s_clients = 0;

/* ─────────────────────── config ─────────────────────── */

static void webfs_copy(char* out, const char* value, size_t max_with_nul) {
    size_t n = strnlen(value, max_with_nul - 1);
    memcpy(out, value, n);
    out[n] = '\0';
}

bool wlan_webfs_config_load(char* ssid_out, char* pw_out) {
    webfs_copy(ssid_out, WLAN_WEBFS_DEFAULT_SSID, WLAN_WEBFS_SSID_MAX + 1);
    webfs_copy(pw_out, WLAN_WEBFS_DEFAULT_PW, WLAN_WEBFS_PW_MAX + 1);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* s = file_stream_alloc(storage);
    if(file_stream_open(s, WEBFS_CONFIG, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FuriString* line = furi_string_alloc();
        while(stream_read_line(s, line)) {
            furi_string_trim(line);
            const char* cstr = furi_string_get_cstr(line);
            if(strncmp(cstr, "ssid=", 5) == 0) {
                webfs_copy(ssid_out, cstr + 5, WLAN_WEBFS_SSID_MAX + 1);
            } else if(strncmp(cstr, "password=", 9) == 0) {
                webfs_copy(pw_out, cstr + 9, WLAN_WEBFS_PW_MAX + 1);
            }
        }
        furi_string_free(line);
    }
    stream_free(s);
    furi_record_close(RECORD_STORAGE);
    return true;
}

bool wlan_webfs_config_save(const char* ssid, const char* pw) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, WEBFS_DIR);
    Stream* s = file_stream_alloc(storage);
    bool ok = false;
    if(file_stream_open(s, WEBFS_CONFIG, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        stream_write_format(s, "ssid=%s\n", ssid);
        stream_write_format(s, "password=%s\n", pw);
        ok = true;
    }
    stream_free(s);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

/* ─────────────────────── HTTP helpers ─────────────────────── */

static int hex_val(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* %XX decode only; '+' stays literal (the UI uses encodeURIComponent). */
static void url_decode(const char* in, char* out, size_t out_max) {
    size_t o = 0;
    for(size_t i = 0; in[i] && o + 1 < out_max;) {
        if(in[i] == '%' && in[i + 1] && in[i + 2]) {
            int h = hex_val(in[i + 1]), l = hex_val(in[i + 2]);
            if(h >= 0 && l >= 0) {
                out[o++] = (char)((h << 4) | l);
                i += 3;
                continue;
            }
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
}

/* Escape ", \ and control chars for a JSON string. */
static void json_escape(char* out, size_t out_size, const char* in) {
    size_t o = 0;
    for(size_t i = 0; in[i] && o + 7 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        if(c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if(c == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if(c == '\r') {
            out[o++] = '\\';
            out[o++] = 'r';
        } else if(c == '\t') {
            out[o++] = '\\';
            out[o++] = 't';
        } else if(c < 0x20) {
            o += snprintf(out + o, out_size - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static bool get_query_param(httpd_req_t* req, const char* key, char* out, size_t out_size) {
    size_t qlen = httpd_req_get_url_query_len(req);
    if(qlen == 0) return false;
    char* q = malloc(qlen + 1);
    if(!q) return false;
    bool ok = false;
    if(httpd_req_get_url_query_str(req, q, qlen + 1) == ESP_OK) {
        char enc[288];
        if(httpd_query_key_value(q, key, enc, sizeof(enc)) == ESP_OK) {
            url_decode(enc, out, out_size);
            ok = true;
        }
    }
    free(q);
    return ok;
}

/* Confine access to /ext (or /ext/...), reject ".." traversal. */
static bool path_ok(const char* p) {
    if(strncmp(p, "/ext", 4) != 0) return false;
    if(p[4] != '\0' && p[4] != '/') return false;
    if(strstr(p, "..")) return false;
    return true;
}

/* ─────────────────────── HTTP handlers ─────────────────────── */

static esp_err_t handler_root(httpd_req_t* req) {
    Storage* st = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(st);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if(storage_file_open(f, WEBFS_INDEX_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint8_t* buf = malloc(WEBFS_IO_CHUNK);
        if(buf) {
            size_t n;
            while((n = storage_file_read(f, buf, WEBFS_IO_CHUNK)) > 0) {
                if(httpd_resp_send_chunk(req, (const char*)buf, n) != ESP_OK) break;
            }
            free(buf);
        }
        httpd_resp_send_chunk(req, NULL, 0);
        storage_file_close(f);
    } else {
        FURI_LOG_W(TAG, "index.html NOT found on SD");
        httpd_resp_sendstr(
            req,
            "<!doctype html><meta charset=utf-8><h2>Web-Filesystem</h2>"
            "<p>Place <code>/ext/webfs/index.html</code> on the SD card.</p>");
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    return ESP_OK;
}

static esp_err_t handler_list(httpd_req_t* req) {
    char path[256];
    if(!get_query_param(req, "path", path, sizeof(path))) strcpy(path, "/ext");
    if(!path_ok(path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }

    Storage* st = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(st);
    if(!storage_dir_open(dir, path)) {
        storage_file_free(dir);
        furi_record_close(RECORD_STORAGE);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not a directory");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    char esc[520];
    char head[560];
    json_escape(esc, sizeof(esc), path);
    int hn = snprintf(head, sizeof(head), "{\"path\":\"%s\",\"entries\":[", esc);
    httpd_resp_send_chunk(req, head, hn);

    FileInfo info;
    char name[256];
    char item[640];
    bool first = true;
    while(storage_dir_read(dir, &info, name, sizeof(name))) {
        if(name[0] == '\0') continue;
        json_escape(esc, sizeof(esc), name);
        bool isdir = file_info_is_dir(&info);
        int in = snprintf(
            item,
            sizeof(item),
            "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%llu}",
            first ? "" : ",",
            esc,
            isdir ? "true" : "false",
            isdir ? 0ULL : (unsigned long long)info.size);
        httpd_resp_send_chunk(req, item, in);
        first = false;
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);

    storage_dir_close(dir);
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
    return ESP_OK;
}

static esp_err_t handler_download(httpd_req_t* req) {
    char path[256];
    if(!get_query_param(req, "path", path, sizeof(path)) || !path_ok(path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }

    Storage* st = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(st);
    if(!storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(f);
        furi_record_close(RECORD_STORAGE);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char cd[300];
    snprintf(cd, sizeof(cd), "attachment; filename=\"%s\"", base);
    httpd_resp_set_hdr(req, "Content-Disposition", cd);

    uint8_t* buf = malloc(WEBFS_IO_CHUNK);
    if(buf) {
        size_t n;
        while((n = storage_file_read(f, buf, WEBFS_IO_CHUNK)) > 0) {
            if(httpd_resp_send_chunk(req, (const char*)buf, n) != ESP_OK) break;
        }
        free(buf);
    }
    httpd_resp_send_chunk(req, NULL, 0);

    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    return ESP_OK;
}

static esp_err_t handler_upload(httpd_req_t* req) {
    char path[256];
    if(!get_query_param(req, "path", path, sizeof(path)) || !path_ok(path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }

    Storage* st = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(st);
    if(!storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(f);
        furi_record_close(RECORD_STORAGE);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_OK;
    }

    uint8_t* buf = malloc(WEBFS_IO_CHUNK);
    bool ok = (buf != NULL);
    int remaining = req->content_len;
    while(ok && remaining > 0) {
        int toread = remaining < WEBFS_IO_CHUNK ? remaining : WEBFS_IO_CHUNK;
        int r = httpd_req_recv(req, (char*)buf, toread);
        if(r <= 0) {
            if(r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ok = false;
            break;
        }
        if(storage_file_write(f, buf, r) != (size_t)r) {
            ok = false;
            break;
        }
        remaining -= r;
    }
    free(buf);
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);

    if(ok) {
        httpd_resp_sendstr(req, "ok");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    }
    return ESP_OK;
}

static esp_err_t handler_rename(httpd_req_t* req) {
    char oldp[256], newp[256];
    if(!get_query_param(req, "old", oldp, sizeof(oldp)) ||
       !get_query_param(req, "new", newp, sizeof(newp)) || !path_ok(oldp) || !path_ok(newp)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }
    Storage* st = furi_record_open(RECORD_STORAGE);
    FS_Error e = storage_common_rename(st, oldp, newp);
    furi_record_close(RECORD_STORAGE);
    if(e == FSE_OK) {
        httpd_resp_sendstr(req, "ok");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, storage_error_get_desc(e));
    }
    return ESP_OK;
}

static esp_err_t handler_delete(httpd_req_t* req) {
    char path[256];
    if(!get_query_param(req, "path", path, sizeof(path)) || !path_ok(path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }
    if(strcmp(path, "/ext") == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "refused");
        return ESP_OK;
    }
    Storage* st = furi_record_open(RECORD_STORAGE);
    bool ok = storage_simply_remove_recursive(st, path);
    furi_record_close(RECORD_STORAGE);
    if(ok) {
        httpd_resp_sendstr(req, "ok");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete failed");
    }
    return ESP_OK;
}

static esp_err_t handler_mkdir(httpd_req_t* req) {
    char path[256];
    if(!get_query_param(req, "path", path, sizeof(path)) || !path_ok(path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }
    Storage* st = furi_record_open(RECORD_STORAGE);
    FS_Error e = storage_common_mkdir(st, path);
    furi_record_close(RECORD_STORAGE);
    if(e == FSE_OK || e == FSE_EXIST) {
        httpd_resp_sendstr(req, "ok");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, storage_error_get_desc(e));
    }
    return ESP_OK;
}

static bool start_http(void) {
    if(s_http) return true;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    /* 6144 stack: the handlers use ~2 KB of local buffers; 4096 overflows the
     * httpd task and the request hangs. */
    config.stack_size = 6144;
    config.max_uri_handlers = 12;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_http, &config);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_http = NULL;
        return false;
    }

    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handler_root},
        {.uri = "/api/list", .method = HTTP_GET, .handler = handler_list},
        {.uri = "/api/download", .method = HTTP_GET, .handler = handler_download},
        {.uri = "/api/upload", .method = HTTP_POST, .handler = handler_upload},
        {.uri = "/api/rename", .method = HTTP_POST, .handler = handler_rename},
        {.uri = "/api/delete", .method = HTTP_POST, .handler = handler_delete},
        {.uri = "/api/mkdir", .method = HTTP_POST, .handler = handler_mkdir},
    };
    for(size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if(httpd_register_uri_handler(s_http, &uris[i]) != ESP_OK) {
            FURI_LOG_W(TAG, "register %s failed", uris[i].uri);
        }
    }
    return true;
}

/* ─────────────────────── AP lifecycle (wlan_hal worker) ─────────────────────── */

static void webfs_wifi_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    (void)data;
    if(base != WIFI_EVENT) return;
    if(id == WIFI_EVENT_AP_STACONNECTED) {
        s_clients++;
    } else if(id == WIFI_EVENT_AP_STADISCONNECTED) {
        if(s_clients > 0) s_clients--;
    }
}

typedef struct {
    const char* ssid;
    const char* pw;
    bool result;
} ApArgs;

static bool s_netif_done = false;

static void webfs_ap_worker(void* arg) {
    ApArgs* sa = arg;
    sa->result = false;

    if(!s_netif_done) {
        esp_netif_init();
        esp_err_t evl = esp_event_loop_create_default();
        if(evl != ESP_OK && evl != ESP_ERR_INVALID_STATE) {
            FURI_LOG_W(TAG, "event_loop: %s", esp_err_to_name(evl));
        }
        s_netif_done = true;
    }

    if(!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if(!s_ap_netif) {
            FURI_LOG_E(TAG, "AP netif alloc failed");
            return;
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num = 2;
    cfg.dynamic_rx_buf_num = 4;
    cfg.dynamic_tx_buf_num = 6;
    if(esp_wifi_init(&cfg) != ESP_OK) {
        FURI_LOG_E(TAG, "wifi_init failed");
        return;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    wifi_config_t ap = {0};
    strncpy((char*)ap.ap.ssid, sa->ssid, 32);
    ap.ap.ssid_len = strlen(sa->ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.beacon_interval = 100;
    if(sa->pw && strlen(sa->pw) >= 8) {
        strncpy((char*)ap.ap.password, sa->pw, 63);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    if(esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK ||
       esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) {
        FURI_LOG_E(TAG, "set_mode/config failed");
        esp_wifi_deinit();
        return;
    }

    if(!s_evt_registered) {
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &webfs_wifi_event, NULL);
        s_evt_registered = true;
    }

    if(esp_wifi_start() != ESP_OK) {
        FURI_LOG_E(TAG, "wifi_start failed");
        esp_wifi_deinit();
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_netif_ip_info_t info;
    if(esp_netif_get_ip_info(s_ap_netif, &info) == ESP_OK) {
        uint32_t ip = info.ip.addr;
        snprintf(
            s_ip,
            sizeof(s_ip),
            "%u.%u.%u.%u",
            (unsigned)(ip & 0xff),
            (unsigned)((ip >> 8) & 0xff),
            (unsigned)((ip >> 16) & 0xff),
            (unsigned)((ip >> 24) & 0xff));
    } else {
        strcpy(s_ip, "192.168.4.1");
    }
    s_clients = 0;

    if(!start_http()) {
        esp_wifi_stop();
        esp_wifi_deinit();
        return;
    }

    s_running = true;
    s_is_ap = true;
    sa->result = true;
    FURI_LOG_I(TAG, "Web-FS AP active ssid='%s' ip=%s", sa->ssid, s_ip);
}

static void webfs_ap_stop_worker(void* arg) {
    (void)arg;
    if(s_http) {
        httpd_stop(s_http);
        s_http = NULL;
    }
    if(s_evt_registered) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &webfs_wifi_event);
        s_evt_registered = false;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    if(s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    s_running = false;
    s_clients = 0;
}

/* ─────────────────────── STA lifecycle (wlan_hal worker) ─────────────────────── */

typedef struct {
    bool result;
} StaArgs;

static void webfs_sta_worker(void* arg) {
    StaArgs* sa = arg;
    sa->result = start_http();
    if(sa->result) {
        s_running = true;
        s_is_ap = false;
    }
}

static void webfs_http_stop_worker(void* arg) {
    (void)arg;
    if(s_http) {
        httpd_stop(s_http);
        s_http = NULL;
    }
    s_running = false;
}

/* ─────────────────────── public API ─────────────────────── */

bool wlan_webfs_start_ap(const char* ssid, const char* password) {
    if(s_running) return true;
    if(!ssid || !ssid[0]) return false;

    /* Take over the radio: stop STA + BLE. */
    if(wlan_hal_is_started()) {
        wlan_hal_stop();
    }
    Bt* bt = furi_record_open(RECORD_BT);
    s_bt_was_on = bt_is_enabled(bt);
    if(s_bt_was_on) bt_stop_stack(bt);
    furi_record_close(RECORD_BT);

    ApArgs sa = {.ssid = ssid, .pw = password, .result = false};
    if(!wlan_hal_run_in_worker(webfs_ap_worker, &sa)) sa.result = false;

    if(!sa.result && s_bt_was_on) {
        Bt* bt2 = furi_record_open(RECORD_BT);
        bt_start_stack(bt2);
        furi_record_close(RECORD_BT);
        s_bt_was_on = false;
    }
    return sa.result;
}

bool wlan_webfs_start_sta(void) {
    if(s_running) return true;
    if(!wlan_hal_is_connected()) return false;

    /* Radio + BLE are already owned by wlan_hal; only start the server. */
    StaArgs sa = {.result = false};
    if(!wlan_hal_run_in_worker(webfs_sta_worker, &sa)) return false;
    if(sa.result) {
        uint32_t ip = wlan_hal_get_own_ip();
        snprintf(
            s_ip,
            sizeof(s_ip),
            "%u.%u.%u.%u",
            (unsigned)(ip & 0xff),
            (unsigned)((ip >> 8) & 0xff),
            (unsigned)((ip >> 16) & 0xff),
            (unsigned)((ip >> 24) & 0xff));
        s_clients = 0;
    }
    return sa.result;
}

void wlan_webfs_stop(void) {
    if(!s_running) return;

    if(s_is_ap) {
        /* Fully tear down httpd + WiFi FIRST so the (large) BLE stack has room to
         * come back — restoring BLE while WiFi/httpd still hold internal DRAM can
         * OOM the Bluedroid workqueue and panic. */
        wlan_hal_run_in_worker(webfs_ap_stop_worker, NULL);
        if(s_bt_was_on) {
            Bt* bt = furi_record_open(RECORD_BT);
            bt_start_stack(bt);
            furi_record_close(RECORD_BT);
            s_bt_was_on = false;
        }
    } else {
        /* STA mode: leave the wlan_hal connection + BLE alone, just stop httpd. */
        wlan_hal_run_in_worker(webfs_http_stop_worker, NULL);
    }
}

bool wlan_webfs_is_running(void) {
    return s_running;
}

bool wlan_webfs_is_ap(void) {
    return s_is_ap;
}

bool wlan_webfs_get_ip(char* out, size_t len) {
    if(!s_running || !out || len < 8) return false;
    strncpy(out, s_ip, len - 1);
    out[len - 1] = '\0';
    return true;
}

uint8_t wlan_webfs_get_client_count(void) {
    int n = s_clients;
    if(n < 0) n = 0;
    if(n > 255) n = 255;
    return (uint8_t)n;
}
