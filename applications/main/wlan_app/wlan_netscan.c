#include "wlan_netscan.h"
#include "wlan_hal.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/etharp.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/sockets.h>
#include <lwip/tcpip.h>
#include <string.h>
#include <furi.h>

#define TAG "WlanNetscan"
#define WLAN_NETSCAN_MAX_HOSTS 64

static WlanNetscanHost s_hosts[WLAN_NETSCAN_MAX_HOSTS];
static uint8_t s_host_count = 0;
static uint32_t s_base_host = 0;
static uint32_t s_own_ip = 0;
static int s_scan_idx = 1;
static bool s_scan_done = false;

static volatile bool s_hostname_running = false;
static volatile bool s_hostname_stop = false;
static TaskHandle_t s_hostname_task = NULL;

static bool host_exists(uint32_t ip) {
    for(int i = 0; i < s_host_count; i++) {
        if(s_hosts[i].ip == ip) return true;
    }
    return false;
}

static void host_add(uint32_t ip, const uint8_t* mac) {
    if(s_host_count >= WLAN_NETSCAN_MAX_HOSTS) return;
    if(host_exists(ip)) return;
    s_hosts[s_host_count].ip = ip;
    memcpy(s_hosts[s_host_count].mac, mac, 6);
    s_host_count++;
}

// Läuft im tcpip-Thread (via tcpip_callback): liest ARP-Tabelle, sendet Probes.
static void arp_scan_tick_fn(void* ctx) {
    UNUSED(ctx);
    struct netif* nif = netif_default;
    if(!nif) return;

    for(size_t idx = 0; idx < ARP_TABLE_SIZE; idx++) {
        ip4_addr_t* ip_ret = NULL;
        struct netif* nif_ret = NULL;
        struct eth_addr* eth_ret = NULL;
        if(etharp_get_entry(idx, &ip_ret, &nif_ret, &eth_ret) == 1) {
            uint32_t found_ip = ip_ret->addr;
            if(found_ip == 0 || found_ip == s_own_ip) continue;
            if(!host_exists(found_ip)) {
                host_add(found_ip, eth_ret->addr);
            }
        }
    }

    if(s_scan_idx <= 254) {
        for(int i = 0; i < 4 && s_scan_idx <= 254; i++, s_scan_idx++) {
            ip4_addr_t target;
            target.addr = htonl(s_base_host | s_scan_idx);
            etharp_request(nif, &target);
        }
    }

    if(s_scan_idx > 254 && !s_scan_done) {
        s_scan_done = true;
    }
}

void wlan_netscan_reset(void) {
    memset(s_hosts, 0, sizeof(s_hosts));
    s_host_count = 0;
    s_scan_done = false;
    s_scan_idx = 1;

    s_own_ip = wlan_hal_get_own_ip();
    uint32_t mask = wlan_hal_get_netmask();
    s_base_host = (s_own_ip && mask) ? ntohl(s_own_ip & mask) : 0;
    // Immer bei .1 anfangen — Probes an die eigene IP werden im
    // etharp_get_entry-Loop sowieso ignoriert.
}

bool wlan_netscan_arp_step(void) {
    if(s_base_host == 0) return true; // kein netif/IP → nichts zu tun
    tcpip_callback(arp_scan_tick_fn, NULL);
    return s_scan_done;
}

uint8_t wlan_netscan_get_host_count(void) {
    return s_host_count;
}

bool wlan_netscan_get_host(uint8_t idx, WlanNetscanHost* out) {
    if(idx >= s_host_count || !out) return false;
    *out = s_hosts[idx];
    return true;
}

uint8_t wlan_netscan_get_progress(void) {
    if(s_base_host == 0) return 100;
    if(s_scan_done) return 100;
    int pct = (s_scan_idx * 100) / 254;
    if(pct < 0) pct = 0;
    if(pct > 100) pct = 100;
    return (uint8_t)pct;
}

// ---------- NetBIOS-Hostname-Resolution (analog wifi_app/netcut_hostname) ----------

static const uint8_t nbstat_query[50] = {
    0x12, 0x34,
    0x00, 0x10,
    0x00, 0x01,
    0x00, 0x00,
    0x00, 0x00,
    0x00, 0x00,
    0x20,
    'C','K','A','A','A','A','A','A','A','A','A','A','A','A','A','A',
    'A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A',
    0x00,
    0x00, 0x21,
    0x00, 0x01,
};

static int skip_dns_name(const uint8_t* buf, int off, int len) {
    int safety = 64;
    while(off < len && safety-- > 0) {
        uint8_t b = buf[off];
        if(b == 0) return off + 1;
        if((b & 0xC0) == 0xC0) return off + 2;
        if(b > 63) return -1;
        off += 1 + b;
    }
    return -1;
}

// DNS-encoded Name dekodieren (inkl. Compression-Pointer). Schreibt
// "label1.label2..." nach out. Gibt true bei mind. 1 Label zurück.
static bool decode_dns_name(const uint8_t* buf, int off, int len, char* out, size_t cap) {
    if(cap == 0) return false;
    size_t j = 0;
    int hops = 16; // max 16 compression hops
    while(off >= 0 && off < len && hops > 0) {
        uint8_t b = buf[off];
        if(b == 0) break;
        if((b & 0xC0) == 0xC0) {
            if(off + 1 >= len) { out[j] = 0; return j > 0; }
            int target = ((b & 0x3F) << 8) | buf[off + 1];
            off = target;
            hops--;
            continue;
        }
        if(b > 63 || off + 1 + b > len) { out[j] = 0; return j > 0; }
        if(j > 0 && j < cap - 1) out[j++] = '.';
        size_t copy = b;
        if(j + copy >= cap) copy = (cap > 0) ? cap - 1 - j : 0;
        memcpy(&out[j], &buf[off + 1], copy);
        j += copy;
        off += 1 + b;
    }
    out[j < cap ? j : cap - 1] = 0;
    return j > 0;
}

// mDNS-Reverse-PTR-Query unicast an die Ziel-IP auf Port 5353.
// Funktioniert für die meisten Apple-Geräte (Bonjour) und einige Linux-Hosts
// mit avahi.
static bool mdns_reverse_resolve(uint32_t ip_be, char* out, size_t cap) {
    if(!out || cap == 0) return false;
    out[0] = '\0';

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(sock < 0) return false;
    struct timeval tv = {.tv_sec = 0, .tv_usec = 350 * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Query-Paket: DNS-Header + QNAME (<d>.<c>.<b>.<a>.in-addr.arpa.) + QTYPE/QCLASS.
    uint8_t pkt[80];
    int p = 0;
    pkt[p++] = 0xAB; pkt[p++] = 0xCD;     // ID
    pkt[p++] = 0x00; pkt[p++] = 0x00;     // Flags: standard query
    pkt[p++] = 0x00; pkt[p++] = 0x01;     // QDCOUNT
    pkt[p++] = 0x00; pkt[p++] = 0x00;     // ANCOUNT
    pkt[p++] = 0x00; pkt[p++] = 0x00;     // NSCOUNT
    pkt[p++] = 0x00; pkt[p++] = 0x00;     // ARCOUNT

    const uint8_t* ipb = (const uint8_t*)&ip_be;
    for(int i = 3; i >= 0; --i) {
        char octet[4];
        int n = snprintf(octet, sizeof(octet), "%u", ipb[i]);
        if(n < 1 || n > 3) { close(sock); return false; }
        pkt[p++] = (uint8_t)n;
        memcpy(&pkt[p], octet, n); p += n;
    }
    pkt[p++] = 7; memcpy(&pkt[p], "in-addr", 7); p += 7;
    pkt[p++] = 4; memcpy(&pkt[p], "arpa", 4); p += 4;
    pkt[p++] = 0;
    pkt[p++] = 0x00; pkt[p++] = 0x0C;     // QTYPE = PTR
    pkt[p++] = 0x00; pkt[p++] = 0x01;     // QCLASS = IN

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(5353);
    dest.sin_addr.s_addr = ip_be;

    if(sendto(sock, pkt, p, 0, (struct sockaddr*)&dest, sizeof(dest)) <= 0) {
        close(sock);
        return false;
    }

    uint8_t buf[256];
    struct sockaddr_in from = {0};
    socklen_t from_len = sizeof(from);
    int rn = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &from_len);
    close(sock);

    if(rn <= 12) return false;
    if(from.sin_addr.s_addr != ip_be) return false;

    uint16_t qcount = ((uint16_t)buf[4] << 8) | buf[5];
    uint16_t acount = ((uint16_t)buf[6] << 8) | buf[7];
    if(acount < 1) return false;

    int off = 12;
    for(int i = 0; i < qcount; i++) {
        off = skip_dns_name(buf, off, rn);
        if(off < 0 || off + 4 > rn) return false;
        off += 4;
    }
    for(int i = 0; i < acount; i++) {
        off = skip_dns_name(buf, off, rn);
        if(off < 0 || off + 10 > rn) return false;
        uint16_t atype = ((uint16_t)buf[off] << 8) | buf[off + 1];
        uint16_t rdlen = ((uint16_t)buf[off + 8] << 8) | buf[off + 9];
        off += 10;
        if(off + rdlen > rn) return false;
        if(atype == 12) {
            char name[64];
            if(!decode_dns_name(buf, off, rn, name, sizeof(name))) return false;
            // ".local"-Suffix entfernen, falls vorhanden.
            size_t nlen = strlen(name);
            if(nlen > 6 && strcasecmp(name + nlen - 6, ".local") == 0) {
                name[nlen - 6] = 0;
            }
            // Leere Hostnames verwerfen.
            if(name[0] == 0) return false;
            // Filter unsichere Zeichen.
            for(size_t k = 0; name[k]; k++) {
                if(name[k] < 0x20 || name[k] >= 0x7f) name[k] = '?';
            }
            strncpy(out, name, cap - 1);
            out[cap - 1] = 0;
            return true;
        }
        off += rdlen;
    }
    return false;
}

static bool parse_nbstat_response(const uint8_t* buf, int len, char* out, size_t cap) {
    if(len < 12) return false;

    uint16_t qcount = ((uint16_t)buf[4] << 8) | buf[5];
    uint16_t acount = ((uint16_t)buf[6] << 8) | buf[7];
    if(acount < 1) return false;

    int off = 12;
    for(int i = 0; i < qcount; i++) {
        off = skip_dns_name(buf, off, len);
        if(off < 0 || off + 4 > len) return false;
        off += 4;
    }

    for(int i = 0; i < acount; i++) {
        off = skip_dns_name(buf, off, len);
        if(off < 0 || off + 10 > len) return false;
        uint16_t atype = ((uint16_t)buf[off] << 8) | buf[off + 1];
        uint16_t rdlen = ((uint16_t)buf[off + 8] << 8) | buf[off + 9];
        off += 10;
        if(off + rdlen > len) return false;

        if(atype == 0x0021) {
            if(rdlen < 1) return false;
            int num_names = buf[off];
            int entries_off = off + 1;

            for(int j = 0; j < num_names; j++) {
                int entry_off = entries_off + j * 18;
                if(entry_off + 18 > off + rdlen) break;
                const uint8_t* n = &buf[entry_off];
                uint16_t flags = ((uint16_t)n[16] << 8) | n[17];
                uint8_t type = n[15];
                bool is_group = (flags & 0x8000) != 0;
                if(!is_group && (type == 0x00 || type == 0x20)) {
                    int nlen = 15;
                    while(nlen > 0 && (n[nlen - 1] == ' ' || n[nlen - 1] == '\0')) nlen--;
                    if(nlen > 0) {
                        size_t copy = (size_t)nlen < cap - 1 ? (size_t)nlen : cap - 1;
                        memcpy(out, n, copy);
                        out[copy] = '\0';
                        for(size_t k = 0; k < copy; k++) {
                            if(out[k] < 0x20 || out[k] >= 0x7f) out[k] = '?';
                        }
                        return true;
                    }
                }
            }
            return false;
        }
        off += rdlen;
    }
    return false;
}

static bool nbstat_resolve(uint32_t ip_be, char* out, size_t cap) {
    if(!out || cap == 0) return false;
    out[0] = '\0';

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(sock < 0) return false;

    struct timeval tv = {.tv_sec = 0, .tv_usec = 350 * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(137);
    dest.sin_addr.s_addr = ip_be;

    int sent = sendto(sock, nbstat_query, sizeof(nbstat_query), 0,
                      (struct sockaddr*)&dest, sizeof(dest));
    if(sent <= 0) {
        close(sock);
        return false;
    }

    uint8_t recv_buf[256];
    struct sockaddr_in from = {0};
    socklen_t from_len = sizeof(from);
    int n = recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                     (struct sockaddr*)&from, &from_len);
    close(sock);

    if(n <= 0) return false;
    if(from.sin_addr.s_addr != ip_be) return false;
    return parse_nbstat_response(recv_buf, n, out, cap);
}

// xTaskCreate-Worker für die Hostname-Resolution. lwIP-Sockets dürfen nur aus
// echten FreeRTOS-Tasks (kein FuriThread) aufgerufen werden.
static void hostname_worker_fn(void* arg) {
    UNUSED(arg);
    s_hostname_running = true;
    for(uint8_t i = 0; i < s_host_count; ++i) {
        if(s_hostname_stop) break;
        if(s_hosts[i].hostname[0]) continue;
        char tmp[24];

        // 1. NetBIOS (Windows / Linux smbd / einige Drucker)
        bool ok = nbstat_resolve(s_hosts[i].ip, tmp, sizeof(tmp));
        // 2. mDNS-Reverse (Apple-Geräte / Linux mit avahi)
        if(!ok) ok = mdns_reverse_resolve(s_hosts[i].ip, tmp, sizeof(tmp));

        if(ok) {
            strncpy(s_hosts[i].hostname, tmp, sizeof(s_hosts[i].hostname) - 1);
            s_hosts[i].hostname[sizeof(s_hosts[i].hostname) - 1] = 0;
        } else {
            // 3. Default-Namen für typische Subnet-Hosts.
            uint8_t host_octet = (uint8_t)((s_hosts[i].ip >> 24) & 0xFF);
            if(host_octet == 1) {
                strncpy(s_hosts[i].hostname, "Router", sizeof(s_hosts[i].hostname) - 1);
                s_hosts[i].hostname[sizeof(s_hosts[i].hostname) - 1] = 0;
            }
        }
    }
    s_hostname_running = false;
    s_hostname_task = NULL;
    vTaskDelete(NULL);
}

void wlan_netscan_start_hostname_resolve(void) {
    if(s_hostname_task) return;
    s_hostname_stop = false;
    xTaskCreate(hostname_worker_fn, "WlanHostHN", 4096, NULL, 4, &s_hostname_task);
}

bool wlan_netscan_hostname_done(void) {
    return !s_hostname_running && s_hostname_task == NULL;
}

void wlan_netscan_stop_hostname_resolve(void) {
    if(!s_hostname_task) return;
    s_hostname_stop = true;
    while(s_hostname_task) {
        furi_delay_ms(20);
    }
}
