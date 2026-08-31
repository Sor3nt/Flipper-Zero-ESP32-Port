#include "airplay_mdns.h"
#include <wlan_hal.h>

#include <lwip/sockets.h>
#include <esp_log.h>
#include <furi.h>
#include <string.h>
#include <stdlib.h>

#define TAG "AirplayMdns"

#define MDNS_PORT       5353
#define MDNS_ADDR_BE    {224, 0, 0, 251} /* 224.0.0.251, already network order */
#define DNS_T_A         1
#define DNS_T_PTR       12
#define DNS_T_TXT       16
#define DNS_T_SRV       33
#define RECV_BUF_SZ     1500

/* host (LE on ESP32) -> network byte order for a 16-bit value */
static inline uint16_t nbo16(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}

/* Parse a DNS name at `pos` in pkt[0..len). Writes the dotted form into out.
 * Returns the stream offset just past the name (compression pointers do not
 * advance the stream beyond their 2 bytes). -1 on malformed input. */
static int dns_read_name(const uint8_t* pkt, int len, int pos, char* out, int outsz) {
    int outlen = 0;
    int jumped = 0;
    int next = -1;
    int hops = 0;
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

/* Extract the friendly device name from a "<instance>._raop._tcp.local" fqdn.
 * The instance label is often "<MAC>@<Name>"; take the part after '@'. */
static void extract_friendly(const char* fqdn, char* out, int outsz) {
    char inst[AIRPLAY_NAME_MAX * 2];
    strncpy(inst, fqdn, sizeof(inst) - 1);
    inst[sizeof(inst) - 1] = '\0';
    char* svc = strstr(inst, "._raop");
    if(svc) *svc = '\0';
    char* at = strchr(inst, '@');
    const char* name = at ? at + 1 : inst;
    strncpy(out, name, outsz - 1);
    out[outsz - 1] = '\0';
}

/* Parse "et=", "cn=", "pw=" out of a TXT rdata block. */
static void parse_txt(const uint8_t* rd, int rdlen, AirplayDevice* dev) {
    int i = 0;
    while(i < rdlen) {
        int l = rd[i++];
        if(l <= 0 || i + l > rdlen) break;
        char kv[64];
        int n = (l < (int)sizeof(kv) - 1) ? l : (int)sizeof(kv) - 1;
        memcpy(kv, &rd[i], n);
        kv[n] = '\0';
        i += l;
        if(!strncmp(kv, "et=", 3)) {
            dev->et = atoi(kv + 3);
        } else if(!strncmp(kv, "cn=", 3)) {
            dev->cn = atoi(kv + 3);
        } else if(!strncmp(kv, "pw=", 3)) {
            dev->needs_password = (kv[3] == 't' || kv[3] == 'T' || kv[3] == '1');
        }
    }
}

/* --- worker-side scan --- */

typedef struct {
    AirplayDevice* out;
    int max;
    uint32_t timeout_ms;
    int count;
} MdnsScanCtx;

static int find_or_add(MdnsScanCtx* c, uint32_t ip) {
    for(int i = 0; i < c->count; i++) {
        if(c->out[i].ip == ip) return i;
    }
    if(c->count >= c->max) return -1;
    int idx = c->count++;
    memset(&c->out[idx], 0, sizeof(AirplayDevice));
    c->out[idx].ip = ip;
    c->out[idx].et = -1;
    c->out[idx].cn = -1;
    return idx;
}

static void build_query(uint8_t* buf, int* out_len) {
    int p = 0;
    /* header: id=0, flags=0, qd=1, an=0, ns=0, ar=0 */
    buf[p++] = 0; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 1;
    buf[p++] = 0; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 0;
    /* qname: _raop._tcp.local */
    static const char* labels[] = {"_raop", "_tcp", "local"};
    for(int i = 0; i < 3; i++) {
        int ll = strlen(labels[i]);
        buf[p++] = (uint8_t)ll;
        memcpy(&buf[p], labels[i], ll);
        p += ll;
    }
    buf[p++] = 0; /* root */
    /* qtype=PTR(12); qclass=IN(1) with the top "QU" bit set (0x8000) to request
     * a UNICAST reply. This firmware has no IGMP, so we cannot receive multicast
     * answers — QU makes the responder answer directly to our source port. */
    buf[p++] = 0; buf[p++] = DNS_T_PTR;
    buf[p++] = 0x80; buf[p++] = 0x01;
    *out_len = p;
}

static void parse_response(const uint8_t* pkt, int len, uint32_t src_ip, MdnsScanCtx* c) {
    if(len < 12) return;
    int an = (pkt[6] << 8) | pkt[7];
    int ns = (pkt[8] << 8) | pkt[9];
    int ar = (pkt[10] << 8) | pkt[11];
    int total = an + ns + ar;

    /* skip question section */
    int qd = (pkt[4] << 8) | pkt[5];
    int pos = 12;
    char name[128];
    for(int i = 0; i < qd; i++) {
        pos = dns_read_name(pkt, len, pos, name, sizeof(name));
        if(pos < 0 || pos + 4 > len) return;
        pos += 4; /* qtype + qclass */
    }

    /* Answer/authority/additional records */
    for(int i = 0; i < total; i++) {
        pos = dns_read_name(pkt, len, pos, name, sizeof(name));
        if(pos < 0 || pos + 10 > len) return;
        int type = (pkt[pos] << 8) | pkt[pos + 1];
        int rdlen = (pkt[pos + 8] << 8) | pkt[pos + 9];
        int rdata = pos + 10;
        if(rdata + rdlen > len) return;

        if(type == DNS_T_SRV) {
            /* name = "<instance>._raop._tcp.local"; rdata: prio(2) wt(2) port(2) target */
            int idx = find_or_add(c, src_ip);
            if(idx >= 0 && rdlen >= 6) {
                uint16_t port = (pkt[rdata + 4] << 8) | pkt[rdata + 5];
                c->out[idx].port = port;
                if(c->out[idx].name[0] == '\0') {
                    extract_friendly(name, c->out[idx].name, AIRPLAY_NAME_MAX);
                }
            }
        } else if(type == DNS_T_TXT) {
            int idx = find_or_add(c, src_ip);
            if(idx >= 0) {
                parse_txt(&pkt[rdata], rdlen, &c->out[idx]);
            }
        } else if(type == DNS_T_PTR) {
            /* rdata is the instance fqdn; keep a name even if SRV is elsewhere */
            char inst[128];
            if(dns_read_name(pkt, len, rdata, inst, sizeof(inst)) > 0) {
                int idx = find_or_add(c, src_ip);
                if(idx >= 0 && c->out[idx].name[0] == '\0') {
                    extract_friendly(inst, c->out[idx].name, AIRPLAY_NAME_MAX);
                }
            }
        }
        pos = rdata + rdlen;
    }
}

static void mdns_scan_worker(void* arg) {
    MdnsScanCtx* c = arg;

    int s = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(s < 0) {
        ESP_LOGE(TAG, "socket failed");
        return;
    }

    uint32_t own_ip = wlan_hal_get_own_ip();

    /* Bind to the STA IP on an ephemeral port: the outgoing multicast query
     * leaves via the STA interface, and the unicast reply (we set the QU bit in
     * the query) comes back here. No multicast RX / IGMP required. */
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    local.sin_addr.s_addr = own_ip;
    if(lwip_bind(s, (struct sockaddr*)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "bind failed");
        lwip_close(s);
        return;
    }

    /* recv timeout so the loop can poll the deadline */
    struct timeval tv = {.tv_sec = 0, .tv_usec = 200 * 1000};
    lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* send the PTR query to 224.0.0.251:5353 */
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = nbo16(MDNS_PORT);
    uint8_t maddr[4] = MDNS_ADDR_BE;
    memcpy(&dst.sin_addr.s_addr, maddr, 4);

    uint8_t query[64];
    int qlen = 0;
    build_query(query, &qlen);
    int sent = lwip_sendto(s, query, qlen, 0, (struct sockaddr*)&dst, sizeof(dst));
    ESP_LOGI(TAG, "mdns query sent=%d own_ip=0x%08x", sent, (unsigned)own_ip);

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
        if(n > 0) {
            ESP_LOGI(TAG, "rx %d bytes from 0x%08x", n, (unsigned)src.sin_addr.s_addr);
            parse_response(buf, n, src.sin_addr.s_addr, c);
        }
        elapsed += 200;
        /* send a second query halfway through for reliability */
        if(!re_queried && elapsed >= c->timeout_ms / 2) {
            lwip_sendto(s, query, qlen, 0, (struct sockaddr*)&dst, sizeof(dst));
            re_queried = true;
        }
    }

    free(buf);
    lwip_close(s);
    ESP_LOGI(TAG, "mdns scan done: %d device(s)", c->count);
    for(int i = 0; i < c->count; i++) {
        ESP_LOGI(
            TAG, "  dev[%d] %s port=%u et=%d cn=%d pw=%d", i, c->out[i].name, c->out[i].port,
            c->out[i].et, c->out[i].cn, c->out[i].needs_password);
    }
}

int airplay_mdns_scan(AirplayDevice* out, int max, uint32_t timeout_ms) {
    MdnsScanCtx ctx = {.out = out, .max = max, .timeout_ms = timeout_ms, .count = 0};
    if(!wlan_hal_run_in_worker(mdns_scan_worker, &ctx)) return 0;
    return ctx.count;
}
