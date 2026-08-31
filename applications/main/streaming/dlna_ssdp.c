#include "dlna_ssdp.h"
#include <esp_attr.h>
#include <wlan_hal.h>

#include <lwip/sockets.h>
#include <esp_log.h>
#include <furi.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#define TAG "DlnaSsdp"

#define SSDP_PORT    1900
#define SSDP_ADDR_BE {239, 255, 255, 250} /* 239.255.255.250, network order */
#define RECV_BUF_SZ  1500
#define DESC_BUF_SZ  8192
#define LOC_MAX      64 /* max unique LOCATION URLs collected before fetching */

static inline uint16_t nbo16(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}

/* ---- case-insensitive substring search ---- */
static const char* stristr(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    if(!nl) return hay;
    for(; *hay; hay++) {
        size_t i = 0;
        while(i < nl && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i]))
            i++;
        if(i == nl) return hay;
    }
    return NULL;
}

/* ---- parse "http://host[:port]/path" ---- */
static bool parse_url(const char* url, uint32_t* out_ip, uint16_t* out_port, char* out_path, int path_sz) {
    if(strncasecmp(url, "http://", 7) != 0) return false;
    const char* p = url + 7;
    char host[64];
    int hi = 0;
    while(*p && *p != ':' && *p != '/' && hi < (int)sizeof(host) - 1) host[hi++] = *p++;
    host[hi] = '\0';
    uint16_t port = 80;
    if(*p == ':') {
        p++;
        port = (uint16_t)atoi(p);
        while(*p && *p != '/') p++;
    }
    /* path (default "/") */
    if(*p == '/') {
        strncpy(out_path, p, path_sz - 1);
        out_path[path_sz - 1] = '\0';
    } else {
        strncpy(out_path, "/", path_sz - 1);
        out_path[path_sz - 1] = '\0';
    }
    uint32_t ip = ipaddr_addr(host); /* dotted-quad → network-order u32 */
    if(ip == IPADDR_NONE) return false;
    *out_ip = ip;
    *out_port = port;
    return true;
}

/* ---- blocking HTTP GET into buf, returns body length (headers stripped) ---- */
static int http_get(uint32_t ip, uint16_t port, const char* path, char* buf, int buf_sz) {
    int s = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(s < 0) return -1;

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = nbo16(port);
    dst.sin_addr.s_addr = ip;

    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    lwip_setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if(lwip_connect(s, (struct sockaddr*)&dst, sizeof(dst)) < 0) {
        lwip_close(s);
        return -1;
    }

    char req[256];
    int rn = snprintf(
        req, sizeof(req),
        "GET %s HTTP/1.1\r\nHOST: %u.%u.%u.%u:%u\r\nCONNECTION: close\r\n"
        "USER-AGENT: FlipperVideo/1.0\r\n\r\n",
        path, (unsigned)(ip & 0xff), (unsigned)((ip >> 8) & 0xff),
        (unsigned)((ip >> 16) & 0xff), (unsigned)((ip >> 24) & 0xff), (unsigned)port);
    if(lwip_send(s, req, rn, 0) < 0) {
        lwip_close(s);
        return -1;
    }

    int total = 0;
    while(total < buf_sz - 1) {
        int n = lwip_recv(s, buf + total, buf_sz - 1 - total, 0);
        if(n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    lwip_close(s);

    /* strip HTTP headers → return body */
    char* body = strstr(buf, "\r\n\r\n");
    if(body) {
        body += 4;
        int blen = total - (int)(body - buf);
        memmove(buf, body, blen);
        buf[blen] = '\0';
        return blen;
    }
    return total;
}

/* ---- extract <tag>...</tag> content into out ---- */
static bool xml_tag(const char* xml, const char* tag, char* out, int out_sz) {
    char open[48], close[48];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char* a = strstr(xml, open);
    if(!a) return false;
    a += strlen(open);
    const char* b = strstr(a, close);
    if(!b) return false;
    /* trim leading whitespace (some devices pretty-print their XML) */
    while(a < b && (*a == ' ' || *a == '\t' || *a == '\r' || *a == '\n')) a++;
    const char* e = b;
    /* trim trailing whitespace */
    while(e > a && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
    int len = (int)(e - a);
    if(len > out_sz - 1) len = out_sz - 1;
    if(len < 0) len = 0;
    memcpy(out, a, len);
    out[len] = '\0';
    return true;
}

/* Resolve a controlURL (absolute http://, root-relative /path, or plain
 * relative) against the device-description base into a path. Sets *out_ip /
 * *out_port when the controlURL is an absolute URL to a different host. */
static void resolve_control(
    const char* ctrl, uint32_t base_ip, uint16_t base_port,
    char* out_path, int path_sz, uint32_t* out_ip, uint16_t* out_port) {
    *out_ip = base_ip;
    *out_port = base_port;
    if(!ctrl[0]) {
        out_path[0] = '\0';
        return;
    }
    if(strncasecmp(ctrl, "http://", 7) == 0) {
        parse_url(ctrl, out_ip, out_port, out_path, path_sz);
    } else if(ctrl[0] == '/') {
        strncpy(out_path, ctrl, path_sz - 1);
        out_path[path_sz - 1] = '\0';
    } else {
        snprintf(out_path, path_sz, "/%s", ctrl);
    }
}

/* Parse the device description: friendlyName + AVTransport / RenderingControl
 * control URLs + DIAL/Cast capability. Returns true if the device is usable as
 * a Cast receiver OR a DLNA renderer. */
static bool parse_device_desc(
    const char* xml, uint32_t base_ip, uint16_t base_port, DlnaDevice* dev) {
    xml_tag(xml, "friendlyName", dev->name, DLNA_NAME_MAX);
    if(!dev->name[0]) strncpy(dev->name, "Media Device", DLNA_NAME_MAX - 1);

    dev->av_control[0] = '\0';
    dev->rc_control[0] = '\0';
    dev->ip = base_ip; /* default; overridden by an AVTransport controlURL host */
    dev->port = base_port;

    /* Cast/DIAL devices advertise the dial-multiscreen service; they are then
     * reachable for CASTV2 on <ip>:8009 regardless of this HTTP port. */
    dev->has_cast = (stristr(xml, "dial-multiscreen-org") != NULL);

    /* Walk each <service> block and match on serviceType. */
    const char* p = xml;
    bool have_av = false;
    while((p = strstr(p, "<service>")) != NULL) {
        const char* end = strstr(p, "</service>");
        if(!end) break;
        int blen = (int)(end - p);
        char block[768];
        int cl = blen < (int)sizeof(block) - 1 ? blen : (int)sizeof(block) - 1;
        memcpy(block, p, cl);
        block[cl] = '\0';

        char stype[96] = {0};
        char curl[DLNA_PATH_MAX] = {0};
        xml_tag(block, "serviceType", stype, sizeof(stype));
        xml_tag(block, "controlURL", curl, sizeof(curl));
        ESP_LOGI(TAG, "    svc type='%.60s' curl='%s'", stype, curl);

        if(curl[0]) {
            char path[DLNA_PATH_MAX];
            uint32_t cip;
            uint16_t cport;
            if(stristr(stype, "AVTransport")) {
                resolve_control(curl, base_ip, base_port, path, sizeof(path), &cip, &cport);
                strncpy(dev->av_control, path, DLNA_PATH_MAX - 1);
                dev->av_control[DLNA_PATH_MAX - 1] = '\0';
                dev->ip = cip;
                dev->port = cport;
                have_av = true;
            } else if(stristr(stype, "RenderingControl")) {
                resolve_control(curl, base_ip, base_port, path, sizeof(path), &cip, &cport);
                strncpy(dev->rc_control, path, DLNA_PATH_MAX - 1);
                dev->rc_control[DLNA_PATH_MAX - 1] = '\0';
            }
        }
        p = end + 10;
    }
    return have_av || dev->has_cast;
}

/* ---- worker-side scan ---- */

typedef struct {
    DlnaDevice* out;
    int max;
    uint32_t timeout_ms;
    int count;
} SsdpCtx;

static void ssdp_scan_worker(void* arg) {
    SsdpCtx* c = arg;

    int s = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(s < 0) {
        ESP_LOGE(TAG, "socket failed");
        return;
    }

    uint32_t own_ip = wlan_hal_get_own_ip();

    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    local.sin_addr.s_addr = own_ip;
    if(lwip_bind(s, (struct sockaddr*)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "bind failed");
        lwip_close(s);
        return;
    }

    struct timeval tv = {.tv_sec = 0, .tv_usec = 250 * 1000};
    lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = nbo16(SSDP_PORT);
    uint8_t maddr[4] = SSDP_ADDR_BE;
    memcpy(&dst.sin_addr.s_addr, maddr, 4);

    /* Force multicast egress out of the STA interface + a TTL that crosses the
     * local switch. Without IP_MULTICAST_IF lwIP may send the M-SEARCH on the
     * wrong/no route → zero replies. */
    struct in_addr mif;
    mif.s_addr = own_ip;
    if(lwip_setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &mif, sizeof(mif)) < 0)
        ESP_LOGW(TAG, "IP_MULTICAST_IF failed");
    uint8_t ttl = 4;
    lwip_setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    /* Query several search targets: ssdp:all catches TVs that ignore the narrow
     * MediaRenderer ST (many do); the others are belt-and-suspenders. We keep
     * only responders that expose an AVTransport controlURL (see parse). MX=2
     * asks responders to spread replies over up to 2 s. Replies come back
     * unicast to our source port (no IGMP RX needed). */
    static const char* const STS[] = {
        "ssdp:all",
        "urn:schemas-upnp-org:device:MediaRenderer:1",
        "urn:schemas-upnp-org:service:AVTransport:1",
    };
    const int n_sts = (int)(sizeof(STS) / sizeof(STS[0]));
    static EXT_RAM_BSS_ATTR char msearch[256]; /* off-stack; worker is serial */

    /* Collect unique LOCATION URLs. */
    char (*locs)[128] = malloc(LOC_MAX * 128);
    int nloc = 0;
    char* rbuf = malloc(RECV_BUF_SZ);
    if(!locs || !rbuf) {
        free(locs);
        free(rbuf);
        lwip_close(s);
        return;
    }

    for(int q = 0; q < n_sts; q++) {
        int ml = snprintf(
            msearch, sizeof(msearch),
            "M-SEARCH * HTTP/1.1\r\n"
            "HOST: 239.255.255.250:1900\r\n"
            "MAN: \"ssdp:discover\"\r\n"
            "MX: 2\r\n"
            "ST: %s\r\n\r\n",
            STS[q]);
        int sent = lwip_sendto(s, msearch, ml, 0, (struct sockaddr*)&dst, sizeof(dst));
        ESP_LOGI(TAG, "M-SEARCH ST=%s sent=%d", STS[q], sent);
    }

    uint32_t elapsed = 0;
    uint32_t last_query = 0;
    while(elapsed < c->timeout_ms && nloc < LOC_MAX) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int n = lwip_recvfrom(s, rbuf, RECV_BUF_SZ - 1, 0, (struct sockaddr*)&src, &sl);
        if(n > 0) {
            rbuf[n] = '\0';
            /* Log the search-target of every reply for diagnosis — this reveals
             * whether an AVTransport/MediaRenderer ever announces itself. */
            char stbuf[80] = "?";
            const char* st = stristr(rbuf, "\nST:");
            if(!st) st = stristr(rbuf, "\nNT:"); /* NOTIFY uses NT instead of ST */
            if(st) {
                st += 4;
                while(*st == ' ') st++;
                int j = 0;
                while(*st && *st != '\r' && *st != '\n' && j < (int)sizeof(stbuf) - 1)
                    stbuf[j++] = *st++;
                stbuf[j] = '\0';
            }
            ESP_LOGI(
                TAG, "rx %d from 0x%08x ST=%s", n, (unsigned)src.sin_addr.s_addr, stbuf);
            const char* loc = stristr(rbuf, "LOCATION:");
            if(loc) {
                loc += 9;
                while(*loc == ' ') loc++;
                char url[128];
                int i = 0;
                while(*loc && *loc != '\r' && *loc != '\n' && i < (int)sizeof(url) - 1)
                    url[i++] = *loc++;
                url[i] = '\0';
                /* dedupe */
                bool seen = false;
                for(int k = 0; k < nloc; k++)
                    if(!strcmp(locs[k], url)) {
                        seen = true;
                        break;
                    }
                if(!seen && url[0]) {
                    strncpy(locs[nloc], url, 127);
                    locs[nloc][127] = '\0';
                    nloc++;
                    ESP_LOGI(TAG, "  location: %s", url);
                }
            }
        }
        elapsed += 250;
        /* Re-send all search targets roughly every 1.2 s (multicast is lossy;
         * a renderer that missed the first burst may answer a later one). */
        if(elapsed - last_query >= 1200) {
            for(int q = 0; q < n_sts; q++) {
                int ml = snprintf(
                    msearch, sizeof(msearch),
                    "M-SEARCH * HTTP/1.1\r\n"
                    "HOST: 239.255.255.250:1900\r\n"
                    "MAN: \"ssdp:discover\"\r\n"
                    "MX: 2\r\n"
                    "ST: %s\r\n\r\n",
                    STS[q]);
                lwip_sendto(s, msearch, ml, 0, (struct sockaddr*)&dst, sizeof(dst));
            }
            last_query = elapsed;
        }
    }
    lwip_close(s);
    ESP_LOGI(TAG, "ssdp collected %d location(s)", nloc);

    /* Fetch + parse each device description. */
    char* desc = malloc(DESC_BUF_SZ);
    if(desc) {
        for(int k = 0; k < nloc && c->count < c->max; k++) {
            uint32_t ip;
            uint16_t port;
            char path[128];
            if(!parse_url(locs[k], &ip, &port, path, sizeof(path))) {
                ESP_LOGW(TAG, "bad location url: %s", locs[k]);
                continue;
            }
            int blen = http_get(ip, port, path, desc, DESC_BUF_SZ);
            ESP_LOGI(TAG, "fetch %s%s -> blen=%d", locs[k], path[0] ? "" : "/", blen);
            if(blen <= 0) {
                ESP_LOGW(TAG, "desc fetch failed: %s", locs[k]);
                continue;
            }
            ESP_LOGI(TAG, "  head: %.140s", desc);
            DlnaDevice dev;
            memset(&dev, 0, sizeof(dev));
            dev.ip = ip;
            dev.port = port;
            bool ok = parse_device_desc(desc, ip, port, &dev);
            ESP_LOGI(
                TAG, "  parsed name='%s' av='%s' rc='%s' ok=%d", dev.name, dev.av_control,
                dev.rc_control, ok);
            if(ok) {
                c->out[c->count++] = dev;
                ESP_LOGI(
                    TAG, "  dev '%s' %u.%u.%u.%u:%u av=%s rc=%s", dev.name,
                    (unsigned)(dev.ip & 0xff), (unsigned)((dev.ip >> 8) & 0xff),
                    (unsigned)((dev.ip >> 16) & 0xff), (unsigned)((dev.ip >> 24) & 0xff),
                    (unsigned)dev.port, dev.av_control, dev.rc_control);
            }
        }
        free(desc);
    }

    free(locs);
    free(rbuf);
    ESP_LOGI(TAG, "ssdp scan done: %d renderer(s)", c->count);
}

int dlna_ssdp_scan(DlnaDevice* out, int max, uint32_t timeout_ms) {
    SsdpCtx ctx = {.out = out, .max = max, .timeout_ms = timeout_ms, .count = 0};
    if(!wlan_hal_run_in_worker(ssdp_scan_worker, &ctx)) return 0;
    return ctx.count;
}
