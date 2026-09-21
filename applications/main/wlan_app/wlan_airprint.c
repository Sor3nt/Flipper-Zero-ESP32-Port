/* AirPrint / IPP-Everywhere virtueller Drucker — Port von fakePrinter/printer.py.
 * Siehe wlan_airprint.h. */

#include "wlan_airprint.h"

#include <wlan_hal.h>
#include <furi.h>
#include <furi_hal_rtc.h>
#include <storage/storage.h>

#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <mdns.h>
#include <esp_mac.h>

#define TAG "AirPrint"

#define AIRPRINT_NAME       "Flipper AirPrint"
#define AIRPRINT_HOSTNAME   "flipper-airprint"
#define AIRPRINT_TASK_STACK 8192

/* ── IPP-Konstanten (RFC 8010/8011), 1:1 aus printer.py ─────────────────── */
#define GRP_OPERATION   0x01
#define GRP_JOB         0x02
#define GRP_END         0x03
#define GRP_PRINTER     0x04

#define TAG_INTEGER     0x21
#define TAG_BOOLEAN     0x22
#define TAG_ENUM        0x23
#define TAG_TEXT        0x41
#define TAG_NAME        0x42
#define TAG_KEYWORD     0x44
#define TAG_URI         0x45
#define TAG_CHARSET     0x47
#define TAG_LANGUAGE    0x48
#define TAG_MIMETYPE    0x49
#define TAG_RESOLUTION  0x32

#define OP_PRINT_JOB              0x0002
#define OP_VALIDATE_JOB          0x0004
#define OP_CREATE_JOB            0x0005
#define OP_SEND_DOCUMENT         0x0006
#define OP_CANCEL_JOB            0x0008
#define OP_GET_JOB_ATTRIBUTES    0x0009
#define OP_GET_JOBS              0x000A
#define OP_GET_PRINTER_ATTRIBUTES 0x000B

#define STATUS_OK                0x0000
#define STATUS_BAD_REQUEST       0x0400
#define STATUS_NOT_SUPPORTED     0x0501

#define JOB_PROCESSING  5
#define JOB_COMPLETED   9

/* ── Modul-Zustand ──────────────────────────────────────────────────────── */
static volatile bool s_running = false;
static volatile bool s_run = false;
static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_done_sem = NULL;
static int s_listen_sock = -1;
static volatile int s_client_sock = -1;
static bool s_mdns_up = false;

static char s_ip[16] = {0};
static char s_uuid[48] = {0};
static char s_name[WLAN_AIRPRINT_NAME_MAX + 1] = AIRPRINT_NAME;
static bool s_name_loaded = false;

/* Spam-Mode: viele Namen aus spam.txt, alle auf dieses Gerät zeigend. */
static bool s_spam = false;
static char s_spam_names[WLAN_AIRPRINT_SPAM_MAX][WLAN_AIRPRINT_NAME_MAX + 1];
static uint16_t s_spam_count = 0;

/* Transienter Anzeigename-Override (Printer-Hijack „Clone"): der beworbene
 * Instanzname wird auf den des Zieldruckers gesetzt, OHNE die persistierte
 * config.txt zu ändern. Aktiv nur während eines Clone-Starts. */
static char s_adv_override[WLAN_AIRPRINT_NAME_MAX + 1];
static bool s_adv_override_on = false;

/* Der aktuell zu bewerbende Anzeigename (Override > persistierter Name). */
static const char* adv_name(void) {
    return s_adv_override_on ? s_adv_override : s_name;
}
static uint32_t s_job_count = 0;
static uint32_t s_next_jid = 0;
static uint32_t s_last_jid = 0;
static char s_last_job_name[64] = {0};
static char s_last_file[64] = {0};

/* ── kleine Byte-Helfer für den IPP-Encoder ─────────────────────────────── */
static void put_u8(uint8_t* b, size_t* p, uint8_t v) {
    b[(*p)++] = v;
}
static void put_u16(uint8_t* b, size_t* p, uint16_t v) {
    b[(*p)++] = (uint8_t)(v >> 8);
    b[(*p)++] = (uint8_t)(v & 0xFF);
}
static void put_u32(uint8_t* b, size_t* p, uint32_t v) {
    b[(*p)++] = (uint8_t)(v >> 24);
    b[(*p)++] = (uint8_t)(v >> 16);
    b[(*p)++] = (uint8_t)(v >> 8);
    b[(*p)++] = (uint8_t)(v & 0xFF);
}
static void put_bytes(uint8_t* b, size_t* p, const void* d, size_t n) {
    memcpy(b + *p, d, n);
    *p += n;
}

/* Attribut mit Namen (erster Wert eines evtl. 1setOf). */
static void attr_str(uint8_t* b, size_t* p, uint8_t tag, const char* name, const char* val) {
    size_t nl = strlen(name), vl = strlen(val);
    put_u8(b, p, tag);
    put_u16(b, p, (uint16_t)nl);
    put_bytes(b, p, name, nl);
    put_u16(b, p, (uint16_t)vl);
    put_bytes(b, p, val, vl);
}
/* Weiterer Wert eines 1setOf (leerer Name). */
static void attr_str_add(uint8_t* b, size_t* p, uint8_t tag, const char* val) {
    size_t vl = strlen(val);
    put_u8(b, p, tag);
    put_u16(b, p, 0);
    put_u16(b, p, (uint16_t)vl);
    put_bytes(b, p, val, vl);
}
static void attr_int(uint8_t* b, size_t* p, uint8_t tag, const char* name, int32_t val) {
    size_t nl = strlen(name);
    put_u8(b, p, tag);
    put_u16(b, p, (uint16_t)nl);
    put_bytes(b, p, name, nl);
    put_u16(b, p, 4);
    put_u32(b, p, (uint32_t)val);
}
static void attr_int_add(uint8_t* b, size_t* p, uint8_t tag, int32_t val) {
    put_u8(b, p, tag);
    put_u16(b, p, 0);
    put_u16(b, p, 4);
    put_u32(b, p, (uint32_t)val);
}
static void attr_bool(uint8_t* b, size_t* p, const char* name, bool val) {
    size_t nl = strlen(name);
    put_u8(b, p, TAG_BOOLEAN);
    put_u16(b, p, (uint16_t)nl);
    put_bytes(b, p, name, nl);
    put_u16(b, p, 1);
    put_u8(b, p, val ? 1 : 0);
}
static void
    attr_res(uint8_t* b, size_t* p, const char* name, int32_t x, int32_t y, uint8_t units) {
    size_t nl = strlen(name);
    put_u8(b, p, TAG_RESOLUTION);
    put_u16(b, p, (uint16_t)nl);
    put_bytes(b, p, name, nl);
    put_u16(b, p, 9);
    put_u32(b, p, (uint32_t)x);
    put_u32(b, p, (uint32_t)y);
    put_u8(b, p, units);
}

/* Response-Kopf: version(2) status(2) request_id(4). */
static void resp_begin(uint8_t* b, size_t* p, uint8_t v0, uint8_t v1, uint16_t status, uint32_t rid) {
    put_u8(b, p, v0);
    put_u8(b, p, v1);
    put_u16(b, p, status);
    put_u32(b, p, rid);
}
static void resp_operation_group(uint8_t* b, size_t* p) {
    put_u8(b, p, GRP_OPERATION);
    attr_str(b, p, TAG_CHARSET, "attributes-charset", "utf-8");
    attr_str(b, p, TAG_LANGUAGE, "attributes-natural-language", "en");
}

/* GRP_PRINTER-Gruppe mit allen AirPrint-Attributen (aus printer_attributes()). */
static void resp_printer_attributes(uint8_t* b, size_t* p) {
    char uri[64];
    snprintf(uri, sizeof(uri), "ipp://%s:%u/ipp/print", s_ip, (unsigned)WLAN_AIRPRINT_PORT);
    char more[48];
    snprintf(more, sizeof(more), "http://%s:%u/", s_ip, (unsigned)WLAN_AIRPRINT_PORT);

    put_u8(b, p, GRP_PRINTER);
    attr_str(b, p, TAG_URI, "printer-uri-supported", uri);
    attr_str(b, p, TAG_KEYWORD, "uri-authentication-supported", "none");
    attr_str(b, p, TAG_KEYWORD, "uri-security-supported", "none");
    attr_str(b, p, TAG_NAME, "printer-name", adv_name());
    attr_str(b, p, TAG_TEXT, "printer-info", adv_name());
    attr_str(b, p, TAG_TEXT, "printer-make-and-model", "Flipper AirPrint 1.0");
    attr_str(b, p, TAG_TEXT, "printer-location", "virtual");
    attr_str(b, p, TAG_URI, "printer-uuid", s_uuid);
    attr_str(b, p, TAG_URI, "printer-more-info", more);
    attr_int(b, p, TAG_ENUM, "printer-state", 3); /* idle */
    attr_str(b, p, TAG_KEYWORD, "printer-state-reasons", "none");
    attr_str(b, p, TAG_KEYWORD, "ipp-versions-supported", "1.1");
    attr_str_add(b, p, TAG_KEYWORD, "2.0");
    attr_str(b, p, TAG_KEYWORD, "ipp-features-supported", "airprint-1.7");
    attr_int(b, p, TAG_ENUM, "operations-supported", OP_PRINT_JOB);
    attr_int_add(b, p, TAG_ENUM, OP_VALIDATE_JOB);
    attr_int_add(b, p, TAG_ENUM, OP_CREATE_JOB);
    attr_int_add(b, p, TAG_ENUM, OP_SEND_DOCUMENT);
    attr_int_add(b, p, TAG_ENUM, OP_CANCEL_JOB);
    attr_int_add(b, p, TAG_ENUM, OP_GET_JOB_ATTRIBUTES);
    attr_int_add(b, p, TAG_ENUM, OP_GET_JOBS);
    attr_int_add(b, p, TAG_ENUM, OP_GET_PRINTER_ATTRIBUTES);
    attr_str(b, p, TAG_CHARSET, "charset-configured", "utf-8");
    attr_str(b, p, TAG_CHARSET, "charset-supported", "utf-8");
    attr_str(b, p, TAG_LANGUAGE, "natural-language-configured", "en");
    attr_str(b, p, TAG_LANGUAGE, "generated-natural-language-supported", "en");
    attr_str(b, p, TAG_MIMETYPE, "document-format-default", "application/pdf");
    attr_str(b, p, TAG_MIMETYPE, "document-format-supported", "application/pdf");
    attr_str_add(b, p, TAG_MIMETYPE, "image/urf");
    attr_str_add(b, p, TAG_MIMETYPE, "image/jpeg");
    attr_str_add(b, p, TAG_MIMETYPE, "image/pwg-raster");
    attr_str_add(b, p, TAG_MIMETYPE, "application/octet-stream");
    attr_bool(b, p, "printer-is-accepting-jobs", true);
    attr_int(b, p, TAG_INTEGER, "queued-job-count", 0);
    attr_str(b, p, TAG_KEYWORD, "pdl-override-supported", "attempted");
    attr_int(b, p, TAG_INTEGER, "printer-up-time", (int32_t)(furi_get_tick() / 1000));
    attr_str(b, p, TAG_KEYWORD, "compression-supported", "none");
    attr_bool(b, p, "color-supported", true);
    attr_str(b, p, TAG_KEYWORD, "print-color-mode-default", "color");
    attr_str(b, p, TAG_KEYWORD, "print-color-mode-supported", "color");
    attr_str_add(b, p, TAG_KEYWORD, "monochrome");
    attr_str(b, p, TAG_KEYWORD, "sides-default", "one-sided");
    attr_str(b, p, TAG_KEYWORD, "sides-supported", "one-sided");
    attr_str_add(b, p, TAG_KEYWORD, "two-sided-long-edge");
    attr_str(b, p, TAG_KEYWORD, "media-default", "iso_a4_210x297mm");
    attr_str(b, p, TAG_KEYWORD, "media-supported", "iso_a4_210x297mm");
    attr_str_add(b, p, TAG_KEYWORD, "na_letter_8.5x11in");
    attr_str(b, p, TAG_KEYWORD, "media-ready", "iso_a4_210x297mm");
    attr_res(b, p, "printer-resolution-default", 300, 300, 3);
    attr_res(b, p, "printer-resolution-supported", 300, 300, 3);
    attr_int(b, p, TAG_ENUM, "print-quality-default", 4);
    attr_int(b, p, TAG_ENUM, "print-quality-supported", 3);
    attr_int_add(b, p, TAG_ENUM, 4);
    attr_int_add(b, p, TAG_ENUM, 5);
    attr_int(b, p, TAG_ENUM, "finishings-default", 3);
    attr_int(b, p, TAG_ENUM, "finishings-supported", 3);
    attr_str(b, p, TAG_KEYWORD, "output-bin-supported", "face-down");
    attr_str(b, p, TAG_KEYWORD, "output-bin-default", "face-down");
    attr_bool(b, p, "multiple-document-jobs-supported", false);
    attr_str(b, p, TAG_KEYWORD, "urf-supported", "CP1");
    attr_str_add(b, p, TAG_KEYWORD, "IS1-5-7");
    attr_str_add(b, p, TAG_KEYWORD, "MT1-3-4-5-8-10-11-12-13");
    attr_str_add(b, p, TAG_KEYWORD, "RS300");
    attr_str_add(b, p, TAG_KEYWORD, "SRGB24");
    attr_str_add(b, p, TAG_KEYWORD, "V1.4");
    attr_str_add(b, p, TAG_KEYWORD, "W8");
    attr_str_add(b, p, TAG_KEYWORD, "DM1");
}

static void resp_job_group(uint8_t* b, size_t* p, uint32_t jid, const char* job_name) {
    char juri[72];
    snprintf(juri, sizeof(juri), "ipp://%s:%u/ipp/print/%u", s_ip, (unsigned)WLAN_AIRPRINT_PORT,
             (unsigned)jid);
    put_u8(b, p, GRP_JOB);
    attr_int(b, p, TAG_INTEGER, "job-id", (int32_t)jid);
    attr_str(b, p, TAG_URI, "job-uri", juri);
    attr_int(b, p, TAG_ENUM, "job-state", JOB_COMPLETED);
    attr_str(b, p, TAG_KEYWORD, "job-state-reasons", "job-completed-successfully");
    attr_str(b, p, TAG_NAME, "job-name", job_name && job_name[0] ? job_name : "job");
}

/* ── gepufferter Socket-Reader (Header + Body über eine Quelle) ─────────── */
typedef struct {
    int sock;
    uint8_t buf[1460];
    size_t len;
    size_t pos;
    bool eof;
} SockBuf;

static void sb_init(SockBuf* sb, int sock) {
    memset(sb, 0, sizeof(*sb));
    sb->sock = sock;
}
/* Füllt den Puffer aus dem Socket, wenn leer. Liefert false bei EOF/Fehler. */
static bool sb_fill(SockBuf* sb) {
    if(sb->pos < sb->len) return true;
    if(sb->eof) return false;
    int n = recv(sb->sock, sb->buf, sizeof(sb->buf), 0);
    if(n <= 0) {
        sb->eof = true;
        return false;
    }
    sb->len = (size_t)n;
    sb->pos = 0;
    return true;
}
/* Ein Byte; -1 bei EOF/Fehler. */
static int sb_getc(SockBuf* sb) {
    if(!sb_fill(sb)) return -1;
    return sb->buf[sb->pos++];
}
/* Bis zu max Bytes; 0 bei EOF. */
static int sb_read(SockBuf* sb, uint8_t* out, size_t max) {
    if(!sb_fill(sb)) return 0;
    size_t avail = sb->len - sb->pos;
    size_t n = avail < max ? avail : max;
    memcpy(out, sb->buf + sb->pos, n);
    sb->pos += n;
    return (int)n;
}

/* ── Body-Reader über SockBuf: Content-Length ODER chunked ──────────────── */
typedef struct {
    SockBuf* sb;
    bool chunked;
    long remaining; /* content-length-Modus: verbleibende Bytes */
    long chunk_left; /* chunked-Modus: Bytes im aktuellen Chunk; -1 = Größe lesen */
    bool done;
} Body;

static void body_init(Body* bd, SockBuf* sb, bool chunked, long content_len) {
    bd->sb = sb;
    bd->chunked = chunked;
    bd->remaining = content_len;
    bd->chunk_left = -1;
    bd->done = false;
}

/* Liest die nächste Chunk-Größenzeile (Hex). Liefert Größe oder -1 bei Fehler.
 * Konsumiert das komplette abschließende CRLF der Größenzeile (sonst landet das
 * \n als erstes Byte der Chunk-Daten und verschiebt den ganzen Stream um 1). */
static long body_read_chunk_size(Body* bd) {
    int c;
    /* Führende CRLF überspringen: das ist das abschließende CRLF der Daten des
     * vorherigen Chunks (beim ersten Chunk gibt es keins). */
    do {
        c = sb_getc(bd->sb);
    } while(c == '\r' || c == '\n');
    if(c < 0) return -1;

    long size = 0;
    bool any = false;
    while(c >= 0 && c != '\r' && c != '\n') {
        int d;
        if(c >= '0' && c <= '9')
            d = c - '0';
        else if(c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if(c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else if(c == ';') {
            /* Chunk-Extension → Rest der Zeile bis \n verwerfen */
            while((c = sb_getc(bd->sb)) >= 0 && c != '\n') {
            }
            any = true;
            return any ? size : -1; /* \n bereits konsumiert */
        } else {
            return -1;
        }
        size = size * 16 + d;
        any = true;
        c = sb_getc(bd->sb);
    }
    /* c ist jetzt '\r' oder '\n'. Bei '\r' noch das folgende '\n' konsumieren. */
    if(c == '\r') sb_getc(bd->sb);
    if(!any) return -1;
    return size;
}

/* Bis zu max Body-Bytes. 0 = Ende, -1 = Fehler. */
static int body_read(Body* bd, uint8_t* out, size_t max) {
    if(bd->done) return 0;
    if(!bd->chunked) {
        if(bd->remaining <= 0) {
            bd->done = true;
            return 0;
        }
        size_t want = (long)max < bd->remaining ? max : (size_t)bd->remaining;
        int n = sb_read(bd->sb, out, want);
        if(n <= 0) {
            bd->done = true;
            return 0;
        }
        bd->remaining -= n;
        return n;
    }
    /* chunked */
    if(bd->chunk_left <= 0) {
        long sz = body_read_chunk_size(bd);
        if(sz < 0) {
            bd->done = true;
            return -1;
        }
        if(sz == 0) {
            /* abschließendes CRLF (Trailer ignoriert) */
            bd->done = true;
            return 0;
        }
        bd->chunk_left = sz;
    }
    size_t want = (long)max < bd->chunk_left ? max : (size_t)bd->chunk_left;
    int n = sb_read(bd->sb, out, want);
    if(n <= 0) {
        bd->done = true;
        return 0;
    }
    bd->chunk_left -= n;
    return n;
}

static void body_drain(Body* bd) {
    uint8_t tmp[512];
    while(body_read(bd, tmp, sizeof(tmp)) > 0) {
    }
}

/* ── IPP-Request-Parsing (nur Attributgruppen; Dokument bleibt Stream) ──── */
typedef enum { IPP_OK, IPP_INCOMPLETE, IPP_ERR } IppParse;

/* Läuft die TLVs ab byte 8 ab, bis GRP_END. Setzt op/rid/job-name/job-id und
 * doc_off (Offset des Dokuments = direkt hinter GRP_END). */
static IppParse ipp_parse(
    const uint8_t* d,
    size_t len,
    uint16_t* op,
    uint32_t* rid,
    char* job_name,
    size_t jn_sz,
    uint32_t* job_id,
    size_t* doc_off) {
    if(len < 8) return IPP_INCOMPLETE;
    *op = (uint16_t)((d[2] << 8) | d[3]);
    *rid = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | d[7];
    size_t pos = 8;
    while(pos < len) {
        uint8_t tag = d[pos++];
        if(tag == GRP_END) {
            *doc_off = pos;
            return IPP_OK;
        }
        if(tag <= 0x0F) continue; /* Gruppen-Delimiter */
        if(pos + 2 > len) return IPP_INCOMPLETE;
        uint16_t nl = (uint16_t)((d[pos] << 8) | d[pos + 1]);
        pos += 2;
        if(pos + nl + 2 > len) return IPP_INCOMPLETE;
        const uint8_t* name = d + pos;
        pos += nl;
        uint16_t vl = (uint16_t)((d[pos] << 8) | d[pos + 1]);
        pos += 2;
        if(pos + vl > len) return IPP_INCOMPLETE;
        const uint8_t* raw = d + pos;
        pos += vl;
        if(nl == 8 && memcmp(name, "job-name", 8) == 0 && tag == TAG_NAME) {
            size_t c = vl < jn_sz - 1 ? vl : jn_sz - 1;
            memcpy(job_name, raw, c);
            job_name[c] = 0;
        } else if(nl == 6 && memcmp(name, "job-id", 6) == 0 && vl == 4) {
            *job_id = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) |
                      ((uint32_t)raw[2] << 8) | raw[3];
        }
    }
    return IPP_INCOMPLETE;
}

/* ── Senden ─────────────────────────────────────────────────────────────── */
static bool send_all(int sock, const void* data, size_t len) {
    const uint8_t* p = data;
    while(len) {
        int n = send(sock, p, len, 0);
        if(n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool send_ipp(int sock, const uint8_t* body, size_t len) {
    char hdr[128];
    int hn = snprintf(
        hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\nContent-Length: %u\r\n"
        "Connection: keep-alive\r\n\r\n",
        (unsigned)len);
    if(!send_all(sock, hdr, (size_t)hn)) return false;
    return send_all(sock, body, len);
}

static void send_simple(int sock, const char* status, const char* body) {
    char hdr[160];
    size_t bl = body ? strlen(body) : 0;
    int hn = snprintf(
        hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\nContent-Type: text/plain\r\nContent-Length: %u\r\n"
        "Connection: keep-alive\r\n\r\n",
        status, (unsigned)bl);
    send_all(sock, hdr, (size_t)hn);
    if(bl) send_all(sock, body, bl);
}

/* ── Dokument speichern (streamt Rest des Body auf die SD) ──────────────── */
static const char* sniff_ext(const uint8_t* p, size_t n) {
    if(n >= 4 && memcmp(p, "%PDF", 4) == 0) return ".pdf";
    if(n >= 2 && p[0] == '%' && p[1] == '!') return ".ps";
    if(n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return ".jpg";
    if(n >= 8 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0) return ".png";
    if(n >= 8 && memcmp(p, "UNIRAST\x00", 8) == 0) return ".urf";
    if(n >= 4 && (memcmp(p, "RaS2", 4) == 0 || memcmp(p, "RaS3", 4) == 0 ||
                  memcmp(p, "PwgR", 4) == 0))
        return ".pwg";
    return ".bin";
}

static void sanitize_name(const char* in, char* out, size_t out_sz) {
    size_t j = 0;
    for(size_t i = 0; in && in[i] && j < out_sz - 1; i++) {
        char c = in[i];
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == ' ' || c == '-' || c == '_')
            out[j++] = c;
    }
    out[j] = 0;
    if(j == 0) strncpy(out, "job", out_sz - 1);
}

/* leftover = bereits gelesene Dokument-Bytes (aus dem Parse-Puffer). */
static void save_document(
    const uint8_t* leftover,
    size_t leftover_len,
    Body* bd,
    const char* job_name) {
    /* Erste Dokumentbytes für die Format-Erkennung besorgen. Bei chunked liegt
     * das Dokument oft komplett hinter GRP_END (leftover==0) → erst ein paar
     * Bytes streamen, bevor die Extension feststeht. */
    uint8_t pre[64];
    size_t pre_from_leftover = leftover_len < sizeof(pre) ? leftover_len : sizeof(pre);
    size_t pre_len = pre_from_leftover;
    if(pre_len) memcpy(pre, leftover, pre_len);
    while(pre_len < 8) {
        int n = body_read(bd, pre + pre_len, sizeof(pre) - pre_len);
        if(n <= 0) break;
        pre_len += (size_t)n;
    }
    const char* ext = sniff_ext(pre, pre_len);
    FURI_LOG_I(TAG, "save: begin ext=%s leftover=%u chunked=%d", ext, (unsigned)leftover_len,
               (int)bd->chunked);

    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    char safe[40];
    sanitize_name(job_name, safe, sizeof(safe));
    char path[128];
    snprintf(
        path, sizeof(path), "%s/%04u%02u%02u_%02u%02u%02u_%s%s", WLAN_AIRPRINT_DIR,
        (unsigned)dt.year, (unsigned)dt.month, (unsigned)dt.day, (unsigned)dt.hour,
        (unsigned)dt.minute, (unsigned)dt.second, safe, ext);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, "/ext/wifi");
    storage_common_mkdir(storage, WLAN_AIRPRINT_DIR);

    File* f = storage_file_alloc(storage);
    if(!storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FURI_LOG_E(TAG, "open failed: %s", path);
        storage_file_free(f);
        furi_record_close(RECORD_STORAGE);
        body_drain(bd);
        return;
    }

    size_t total = 0;
    /* die für die Erkennung gelesenen Bytes zuerst schreiben ... */
    if(pre_len) {
        storage_file_write(f, pre, pre_len);
        total += pre_len;
    }
    /* ... dann die leftover-Bytes, die nicht mehr in pre[] gepasst haben. */
    if(leftover_len > pre_from_leftover) {
        storage_file_write(f, leftover + pre_from_leftover, leftover_len - pre_from_leftover);
        total += leftover_len - pre_from_leftover;
    }
    uint8_t* buf = malloc(2048);
    if(buf) {
        int n;
        while((n = body_read(bd, buf, 2048)) > 0) {
            storage_file_write(f, buf, (size_t)n);
            total += (size_t)n;
        }
        free(buf);
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);

    s_job_count++;
    /* Basename für die UI merken */
    const char* base = strrchr(path, '/');
    strncpy(s_last_file, base ? base + 1 : path, sizeof(s_last_file) - 1);
    s_last_file[sizeof(s_last_file) - 1] = 0;
    FURI_LOG_I(TAG, "job saved: %s (%u B)", path, (unsigned)total);
}

static uint32_t new_job(const char* job_name) {
    s_last_jid = ++s_next_jid;
    strncpy(s_last_job_name, job_name && job_name[0] ? job_name : "job",
            sizeof(s_last_job_name) - 1);
    s_last_job_name[sizeof(s_last_job_name) - 1] = 0;
    return s_last_jid;
}

/* ── einen IPP-POST behandeln ───────────────────────────────────────────── */
static void handle_ipp(int sock, Body* bd) {
    uint8_t* ipp = malloc(4096);
    if(!ipp) {
        send_simple(sock, "500 Internal Server Error", NULL);
        body_drain(bd);
        return;
    }
    size_t ilen = 0;
    uint16_t op = 0;
    uint32_t rid = 0, job_id = 0;
    size_t doc_off = 0;
    char job_name[64] = {0};
    IppParse res = IPP_INCOMPLETE;

    for(;;) {
        res = ipp_parse(ipp, ilen, &op, &rid, job_name, sizeof(job_name), &job_id, &doc_off);
        if(res != IPP_INCOMPLETE) break;
        if(ilen >= 4096) {
            res = IPP_ERR;
            break;
        }
        int n = body_read(bd, ipp + ilen, 4096 - ilen);
        if(n <= 0) break; /* Body zu Ende → nochmal parsen, dann entscheiden */
        ilen += (size_t)n;
    }
    if(res == IPP_INCOMPLETE)
        res = ipp_parse(ipp, ilen, &op, &rid, job_name, sizeof(job_name), &job_id, &doc_off);

    if(res != IPP_OK) {
        FURI_LOG_W(TAG, "bad IPP request (%u bytes)", (unsigned)ilen);
        /* höfliche IPP-Fehlerantwort */
        size_t p = 0;
        resp_begin(ipp, &p, 0x02, 0x00, STATUS_BAD_REQUEST, rid);
        resp_operation_group(ipp, &p);
        put_u8(ipp, &p, GRP_END);
        send_ipp(sock, ipp, p);
        free(ipp);
        return;
    }

    uint8_t v0 = ipp[0], v1 = ipp[1];
    const uint8_t* leftover = ipp + doc_off;
    size_t leftover_len = ilen - doc_off;
    FURI_LOG_I(
        TAG, "ipp op=0x%04x rid=%u job='%s' job_id=%u leftover=%u", op, (unsigned)rid, job_name,
        (unsigned)job_id, (unsigned)leftover_len);

    /* Antwort in einem separaten Puffer bauen (ipp trägt noch das Dokument). */
    uint8_t* out = malloc(4096);
    if(!out) {
        free(ipp);
        return;
    }
    size_t p = 0;

    switch(op) {
    case OP_GET_PRINTER_ATTRIBUTES:
        resp_begin(out, &p, v0, v1, STATUS_OK, rid);
        resp_operation_group(out, &p);
        resp_printer_attributes(out, &p);
        put_u8(out, &p, GRP_END);
        send_ipp(sock, out, p);
        body_drain(bd);
        FURI_LOG_I(TAG, "Get-Printer-Attributes");
        break;

    case OP_VALIDATE_JOB:
        resp_begin(out, &p, v0, v1, STATUS_OK, rid);
        resp_operation_group(out, &p);
        put_u8(out, &p, GRP_END);
        send_ipp(sock, out, p);
        body_drain(bd);
        break;

    case OP_CREATE_JOB: {
        uint32_t jid = new_job(job_name);
        resp_begin(out, &p, v0, v1, STATUS_OK, rid);
        resp_operation_group(out, &p);
        resp_job_group(out, &p, jid, job_name);
        put_u8(out, &p, GRP_END);
        send_ipp(sock, out, p);
        body_drain(bd);
        FURI_LOG_I(TAG, "Create-Job #%u (%s)", (unsigned)jid, job_name);
        break;
    }

    case OP_PRINT_JOB: {
        uint32_t jid = new_job(job_name);
        save_document(leftover, leftover_len, bd, job_name);
        resp_begin(out, &p, v0, v1, STATUS_OK, rid);
        resp_operation_group(out, &p);
        resp_job_group(out, &p, jid, job_name);
        put_u8(out, &p, GRP_END);
        send_ipp(sock, out, p);
        break;
    }

    case OP_SEND_DOCUMENT: {
        uint32_t jid = job_id ? job_id : s_last_jid;
        const char* jn = s_last_job_name[0] ? s_last_job_name : job_name;
        if(leftover_len > 0 || !bd->done) {
            save_document(leftover, leftover_len, bd, jn);
        } else {
            body_drain(bd);
        }
        resp_begin(out, &p, v0, v1, STATUS_OK, rid);
        resp_operation_group(out, &p);
        resp_job_group(out, &p, jid, jn);
        put_u8(out, &p, GRP_END);
        send_ipp(sock, out, p);
        break;
    }

    case OP_GET_JOBS:
    case OP_GET_JOB_ATTRIBUTES: {
        uint32_t jid = job_id ? job_id : (s_last_jid ? s_last_jid : 1);
        resp_begin(out, &p, v0, v1, STATUS_OK, rid);
        resp_operation_group(out, &p);
        resp_job_group(out, &p, jid, s_last_job_name);
        put_u8(out, &p, GRP_END);
        send_ipp(sock, out, p);
        body_drain(bd);
        break;
    }

    case OP_CANCEL_JOB:
        resp_begin(out, &p, v0, v1, STATUS_OK, rid);
        resp_operation_group(out, &p);
        put_u8(out, &p, GRP_END);
        send_ipp(sock, out, p);
        body_drain(bd);
        break;

    default:
        resp_begin(out, &p, v0, v1, STATUS_NOT_SUPPORTED, rid);
        resp_operation_group(out, &p);
        put_u8(out, &p, GRP_END);
        send_ipp(sock, out, p);
        body_drain(bd);
        FURI_LOG_W(TAG, "unsupported op 0x%04x", op);
        break;
    }

    free(out);
    free(ipp);
}

/* ── eine Verbindung (HTTP/1.1 keep-alive) ──────────────────────────────── */
static void handle_connection(int sock) {
    SockBuf* sb = malloc(sizeof(SockBuf));
    if(!sb) return;
    sb_init(sb, sock);

    while(s_run) {
        /* Request-Zeile + Header lesen (bis Leerzeile) */
        char line[512];
        bool is_post = false;
        long content_len = 0;
        bool chunked = false;
        bool expect_continue = false;
        bool conn_close = false;
        bool is_ipp = false;
        bool have_request = false;

        for(int lineno = 0;; lineno++) {
            size_t li = 0;
            int c;
            while((c = sb_getc(sb)) >= 0 && c != '\n') {
                if(c != '\r' && li < sizeof(line) - 1) line[li++] = (char)c;
            }
            if(c < 0 && li == 0) {
                have_request = false;
                goto done; /* Verbindung zu */
            }
            line[li] = 0;
            if(lineno == 0) {
                have_request = true;
                if(strncmp(line, "POST", 4) == 0) is_post = true;
                /* GET/andere: is_post bleibt false */
            } else if(li == 0) {
                break; /* Ende der Header */
            } else {
                if(strncasecmp(line, "Content-Length:", 15) == 0) {
                    content_len = strtol(line + 15, NULL, 10);
                } else if(strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
                    if(strstr(line + 18, "chunked")) chunked = true;
                } else if(strncasecmp(line, "Expect:", 7) == 0) {
                    if(strstr(line + 7, "100-continue")) expect_continue = true;
                } else if(strncasecmp(line, "Connection:", 11) == 0) {
                    if(strstr(line + 11, "close") || strstr(line + 11, "Close")) conn_close = true;
                } else if(strncasecmp(line, "Content-Type:", 13) == 0) {
                    if(strstr(line + 13, "ipp")) is_ipp = true;
                }
            }
            if(c < 0) break;
        }

        if(!have_request) break;

        FURI_LOG_I(
            TAG, "req: %s cl=%ld chunked=%d ipp=%d expect100=%d", is_post ? "POST" : "GET/other",
            content_len, (int)chunked, (int)is_ipp, (int)expect_continue);

        if(is_post) {
            if(expect_continue) {
                const char* cont = "HTTP/1.1 100 Continue\r\n\r\n";
                send_all(sock, cont, strlen(cont));
            }
            Body bd;
            body_init(&bd, sb, chunked, content_len);
            if(is_ipp) {
                handle_ipp(sock, &bd);
            } else {
                body_drain(&bd);
                send_simple(sock, "400 Bad Request", NULL);
            }
        } else {
            /* GET/HEAD o.ä. → Statuszeile */
            send_simple(sock, "200 OK", "Flipper AirPrint running\n");
        }

        if(conn_close) break;
    }

done:
    free(sb);
}

/* ── Server-Task ────────────────────────────────────────────────────────── */
static void server_task(void* arg) {
    (void)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if(ls < 0) {
        FURI_LOG_E(TAG, "socket() failed");
        s_run = false;
        xSemaphoreGive(s_done_sem);
        vTaskDelete(NULL);
        return;
    }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); /* accept() unblockt zyklisch */

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(WLAN_AIRPRINT_PORT);
    if(bind(ls, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        FURI_LOG_E(TAG, "bind(:%u) failed", (unsigned)WLAN_AIRPRINT_PORT);
        close(ls);
        s_run = false;
        xSemaphoreGive(s_done_sem);
        vTaskDelete(NULL);
        return;
    }
    listen(ls, 2);
    s_listen_sock = ls;
    FURI_LOG_I(TAG, "IPP server on %s:%u", s_ip, (unsigned)WLAN_AIRPRINT_PORT);

    while(s_run) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int c = accept(ls, (struct sockaddr*)&cli, &cl);
        if(c < 0) continue; /* Timeout oder Fehler → s_run prüfen */
        if(!s_run) {
            close(c);
            break;
        }
        FURI_LOG_I(TAG, "accept: client %s", inet_ntoa(cli.sin_addr));
        struct timeval ctv = {.tv_sec = 12, .tv_usec = 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &ctv, sizeof(ctv));
        s_client_sock = c;
        handle_connection(c);
        s_client_sock = -1;
        close(c);
        FURI_LOG_I(TAG, "conn closed");
    }

    s_listen_sock = -1;
    close(ls);
    FURI_LOG_I(TAG, "IPP server stopped");
    xSemaphoreGive(s_done_sem);
    vTaskDelete(NULL);
}

/* ── mDNS (über wlan_hal-Worker) ────────────────────────────────────────── */
typedef struct {
    bool ok;
} MdnsCtx;

/* Registriert eine _ipp._tcp-Instanz <name> (Port/Host = dieses Gerät). */
static bool mdns_add_printer(const char* name, const char* uuid_bare, const char* adminurl) {
    mdns_txt_item_t txt[] = {
        {"txtvers", "1"},
        {"qtotal", "1"},
        {"rp", "ipp/print"},
        {"ty", name},
        {"adminurl", adminurl},
        {"note", "virtual"},
        {"product", "(Flipper AirPrint)"},
        {"pdl", "application/pdf,image/urf,image/jpeg"},
        {"URF", "CP1,IS1-5-7,MT1-3-4-5-8-10-11-12-13,RS300,SRGB24,V1.4,W8,DM1"},
        {"Color", "T"},
        {"Duplex", "F"},
        {"UUID", uuid_bare},
        {"TLS", "1.2"},
        {"priority", "50"},
    };
    esp_err_t e = mdns_service_add(name, "_ipp", "_tcp", WLAN_AIRPRINT_PORT, txt,
                                   sizeof(txt) / sizeof(txt[0]));
    if(e != ESP_OK) {
        FURI_LOG_W(TAG, "service_add '%s': %s", name, esp_err_to_name(e));
        return false;
    }
    /* AirPrint-Subtyp (echte AirPrint-Drucker bewerben _universal). */
    mdns_service_subtype_add_for_host(name, "_ipp", "_tcp", NULL, "_universal");
    return true;
}

static void mdns_start_worker(void* arg) {
    MdnsCtx* ctx = arg;
    ctx->ok = false;

    esp_err_t e = mdns_init();
    if(e != ESP_OK) {
        FURI_LOG_E(TAG, "mdns_init: %s", esp_err_to_name(e));
        return;
    }
    mdns_hostname_set(AIRPRINT_HOSTNAME);

    char adminurl[48];
    snprintf(adminurl, sizeof(adminurl), "http://%s:%u/", s_ip, (unsigned)WLAN_AIRPRINT_PORT);

    if(s_spam) {
        uint8_t mac[6] = {0};
        if(!wlan_hal_get_own_mac(mac)) esp_read_mac(mac, ESP_MAC_WIFI_STA);
        int reg = 0;
        for(uint16_t i = 0; i < s_spam_count; i++) {
            char uuid_i[40];
            snprintf(uuid_i, sizeof(uuid_i), "%08x-%02x%02x-4e5f-8a00-%02x%02x%02x%02x%02x%02x",
                     (unsigned)(i + 1), mac[0], mac[1], mac[0], mac[1], mac[2], mac[3], mac[4],
                     mac[5]);
            if(mdns_add_printer(s_spam_names[i], uuid_i, adminurl)) reg++;
        }
        FURI_LOG_I(TAG, "spam: %d/%u printers advertised", reg, (unsigned)s_spam_count);
        ctx->ok = (reg > 0);
        if(!ctx->ok) mdns_free();
        return;
    }

    /* Einzel-Modus (bzw. Clone: adv_name() = geklonter Zieldruckername) */
    mdns_instance_name_set(adv_name());
    char uuid_txt[40];
    const char* up = s_uuid; /* TXT-UUID ohne "urn:uuid:"-Präfix (wie zeroconf) */
    if(strncmp(up, "urn:uuid:", 9) == 0) up += 9;
    strncpy(uuid_txt, up, sizeof(uuid_txt) - 1);
    uuid_txt[sizeof(uuid_txt) - 1] = 0;

    ctx->ok = mdns_add_printer(adv_name(), uuid_txt, adminurl);
    if(!ctx->ok) mdns_free();
}

static void mdns_stop_worker(void* arg) {
    (void)arg;
    mdns_free();
}

/* ── Name-Persistenz (/ext/wifi/printer/config.txt, Zeile 1 = Name) ─────── */
#define AIRPRINT_CONFIG_PATH WLAN_AIRPRINT_DIR "/config.txt"

static void airprint_load_name(void) {
    if(s_name_loaded) return;
    s_name_loaded = true; /* nur einmal versuchen; Default bleibt bei Fehler */

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, AIRPRINT_CONFIG_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[WLAN_AIRPRINT_NAME_MAX + 1] = {0};
        size_t n = storage_file_read(f, buf, sizeof(buf) - 1);
        buf[n] = 0;
        /* erste Zeile trimmen */
        for(size_t i = 0; i < n; i++) {
            if(buf[i] == '\r' || buf[i] == '\n') {
                buf[i] = 0;
                break;
            }
        }
        if(buf[0]) {
            strncpy(s_name, buf, sizeof(s_name) - 1);
            s_name[sizeof(s_name) - 1] = 0;
        }
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}

void wlan_airprint_set_name(const char* name) {
    if(!name || !name[0]) return;
    strncpy(s_name, name, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = 0;
    s_name_loaded = true;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, "/ext/wifi");
    storage_common_mkdir(storage, WLAN_AIRPRINT_DIR);
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, AIRPRINT_CONFIG_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(f, s_name, strlen(s_name));
        storage_file_write(f, "\n", 1);
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    FURI_LOG_I(TAG, "name set: %s", s_name);
}

/* ── Spam-Namen laden (/ext/wifi/printer/spam.txt) ──────────────────────── */
#define AIRPRINT_SPAM_PATH WLAN_AIRPRINT_DIR "/spam.txt"

static void airprint_load_spam_names(void) {
    s_spam_count = 0;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, "/ext/wifi");
    storage_common_mkdir(storage, WLAN_AIRPRINT_DIR);

    File* f = storage_file_alloc(storage);
    if(!storage_file_open(f, AIRPRINT_SPAM_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        /* Fallback: Beispieldatei anlegen (wird nur genutzt, wenn keine da ist). */
        storage_file_close(f);
        if(storage_file_open(f, AIRPRINT_SPAM_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            const char* sample =
                "# Ein Druckername pro Zeile. Zeilen mit # werden ignoriert.\n"
                "HP OfficeJet Pro\n"
                "Canon PIXMA\n"
                "Epson WorkForce\n"
                "Brother HL-2270DW\n"
                "Office Printer\n";
            storage_file_write(f, sample, strlen(sample));
        }
        storage_file_close(f);
        if(!storage_file_open(f, AIRPRINT_SPAM_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
            storage_file_free(f);
            furi_record_close(RECORD_STORAGE);
            return;
        }
    }

    char* buf = malloc(4096);
    if(buf) {
        size_t n = storage_file_read(f, buf, 4095);
        buf[n] = 0;
        /* strtok mit "\r\n" überspringt Leerzeilen automatisch. */
        char* line = strtok(buf, "\r\n");
        while(line && s_spam_count < WLAN_AIRPRINT_SPAM_MAX) {
            while(*line == ' ' || *line == '\t') line++;
            if(*line && *line != '#') {
                /* trailing whitespace trimmen */
                size_t len = strlen(line);
                while(len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) line[--len] = 0;
                strncpy(s_spam_names[s_spam_count], line, WLAN_AIRPRINT_NAME_MAX);
                s_spam_names[s_spam_count][WLAN_AIRPRINT_NAME_MAX] = 0;
                s_spam_count++;
            }
            line = strtok(NULL, "\r\n");
        }
        free(buf);
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    FURI_LOG_I(TAG, "spam: %u names loaded", (unsigned)s_spam_count);
}

uint16_t wlan_airprint_get_spam_count(void) {
    return s_spam ? s_spam_count : 0;
}

/* ── öffentliche API ────────────────────────────────────────────────────── */
static void make_uuid(void) {
    uint8_t mac[6] = {0};
    if(!wlan_hal_get_own_mac(mac)) esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(
        s_uuid, sizeof(s_uuid), "urn:uuid:%02x%02x%02x%02x-%02x%02x-4e5f-8a00-%02x%02x%02x%02x%02x%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], mac[0], mac[1], mac[2], mac[3], mac[4],
        mac[5]);
}

typedef enum { AP_MODE_SINGLE, AP_MODE_SPAM, AP_MODE_CAPTURE } ApStartMode;

static bool airprint_start_common(ApStartMode mode) {
    if(s_running) return true;
    if(!wlan_hal_is_connected()) {
        FURI_LOG_E(TAG, "not connected");
        return false;
    }

    uint32_t ip = wlan_hal_get_own_ip(); /* network byte order (im Capture-Modus = Ziel-IP) */
    if(ip == 0) {
        FURI_LOG_E(TAG, "no IP");
        return false;
    }
    snprintf(s_ip, sizeof(s_ip), "%u.%u.%u.%u", (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
             (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF));
    airprint_load_name();

    s_spam = (mode == AP_MODE_SPAM);
    if(s_spam) {
        airprint_load_spam_names();
        if(s_spam_count == 0) {
            FURI_LOG_E(TAG, "spam: no names in " AIRPRINT_SPAM_PATH);
            return false;
        }
    }
    make_uuid();

    s_job_count = 0;
    s_next_jid = 0;
    s_last_jid = 0;
    s_last_job_name[0] = 0;
    s_last_file[0] = 0;

    /* mDNS im wlan-Worker registrieren (im Capture-Modus NICHT — dort zeigt die
     * übernommene IP bereits auf uns). */
    if(mode != AP_MODE_CAPTURE) {
        MdnsCtx ctx = {0};
        if(!wlan_hal_run_in_worker(mdns_start_worker, &ctx) || !ctx.ok) {
            FURI_LOG_E(TAG, "mDNS start failed");
            return false;
        }
        s_mdns_up = true;
    }

    if(!s_done_sem) s_done_sem = xSemaphoreCreateBinary();
    s_run = true;
    if(xTaskCreate(server_task, "AirPrint", AIRPRINT_TASK_STACK, NULL, 5, &s_task) != pdPASS) {
        FURI_LOG_E(TAG, "task create failed");
        s_run = false;
        if(s_mdns_up) {
            wlan_hal_run_in_worker(mdns_stop_worker, NULL);
            s_mdns_up = false;
        }
        return false;
    }
    s_running = true;
    return true;
}

bool wlan_airprint_start(void) {
    return airprint_start_common(AP_MODE_SINGLE);
}

bool wlan_airprint_start_spam(void) {
    return airprint_start_common(AP_MODE_SPAM);
}

bool wlan_airprint_start_capture(void) {
    return airprint_start_common(AP_MODE_CAPTURE);
}

bool wlan_airprint_start_clone(const char* name) {
    if(name && name[0]) {
        strncpy(s_adv_override, name, sizeof(s_adv_override) - 1);
        s_adv_override[sizeof(s_adv_override) - 1] = 0;
        s_adv_override_on = true;
    }
    bool ok = airprint_start_common(AP_MODE_SINGLE);
    if(!ok) s_adv_override_on = false;
    return ok;
}

void wlan_airprint_stop(void) {
    if(!s_running) return;
    FURI_LOG_I(TAG, "stop: begin");
    s_run = false;
    /* Aktiven Client-Socket aufwecken, falls der Task gerade in recv() blockt
     * (sonst blockt Back bis zum recv-Timeout). shutdown() auf einem VERBUNDENEN
     * Socket ist robust (anders als auf dem Listen-Socket). Der accept-Loop
     * beendet sich über seinen 1s-Timeout. */
    if(s_client_sock >= 0) {
        shutdown(s_client_sock, SHUT_RDWR);
    }
    if(s_done_sem) {
        if(xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(15000)) != pdTRUE) {
            FURI_LOG_E(TAG, "stop: task join TIMEOUT");
        } else {
            FURI_LOG_I(TAG, "stop: task joined");
        }
    }
    s_task = NULL;
    if(s_mdns_up) {
        FURI_LOG_I(TAG, "stop: freeing mDNS");
        wlan_hal_run_in_worker(mdns_stop_worker, NULL);
        s_mdns_up = false;
    }
    s_adv_override_on = false; /* Clone-Override zurücksetzen */
    s_running = false;
    FURI_LOG_I(TAG, "stop: done (%u jobs)", (unsigned)s_job_count);
}

bool wlan_airprint_is_running(void) {
    return s_running;
}

bool wlan_airprint_get_ip(char* out, size_t len) {
    if(!out || len == 0) return false;
    strncpy(out, s_ip, len - 1);
    out[len - 1] = 0;
    return s_ip[0] != 0;
}

uint16_t wlan_airprint_get_port(void) {
    return WLAN_AIRPRINT_PORT;
}

const char* wlan_airprint_get_name(void) {
    airprint_load_name();
    return s_name;
}

uint32_t wlan_airprint_get_job_count(void) {
    return s_job_count;
}

const char* wlan_airprint_get_last_file(void) {
    return s_last_file;
}
