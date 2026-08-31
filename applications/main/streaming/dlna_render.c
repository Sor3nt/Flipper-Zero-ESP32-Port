#include "dlna_render.h"
#include <esp_attr.h>

#include <lwip/sockets.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <furi.h>
#include <storage/storage.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#define TAG "DlnaRender"

#define HTTPD_STACK   4096 /* words */
#define IO_CHUNK      (32 * 1024) /* SD read / socket send chunk — bigger = fewer round-trips */
#define REQ_BUF       1024
#define SOAP_RESP_BUF 2048

/* ---- served-file state (guarded by mutex) ---- */
static FuriMutex* s_file_mtx = NULL;
static EXT_RAM_BSS_ATTR char s_file_path[256] = {0};
static EXT_RAM_BSS_ATTR char s_url_name[128] = {0};

/* ---- httpd task ---- */
static TaskHandle_t s_httpd_task = NULL;
static StackType_t* s_httpd_stack = NULL;
static StaticTask_t* s_httpd_buf = NULL;
static volatile bool s_httpd_run = false;
static uint32_t s_local_ip = 0;
static int s_listen_sock = -1;
static volatile int s_client_sock = -1; /* connection currently being served */

/* IO buffer (single httpd task → no reentry). PSRAM is fine: lwIP copies into
 * its own pbufs on send. */
static uint8_t* s_io_buf = NULL;

static inline uint16_t nbo16(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}

/* ---------------- content-type by extension ---------------- */

const char* dlna_render_mime_for(const char* name) {
    const char* dot = strrchr(name, '.');
    if(!dot) return "application/octet-stream";
    char ext[8];
    int i = 0;
    for(const char* p = dot + 1; *p && i < 7; p++) ext[i++] = tolower((unsigned char)*p);
    ext[i] = '\0';
    if(!strcmp(ext, "mp4") || !strcmp(ext, "m4v")) return "video/mp4";
    if(!strcmp(ext, "mkv")) return "video/x-matroska";
    if(!strcmp(ext, "avi")) return "video/x-msvideo";
    if(!strcmp(ext, "mov")) return "video/quicktime";
    if(!strcmp(ext, "webm")) return "video/webm";
    if(!strcmp(ext, "ts")) return "video/mp2t";
    if(!strcmp(ext, "mpg") || !strcmp(ext, "mpeg")) return "video/mpeg";
    if(!strcmp(ext, "wmv")) return "video/x-ms-wmv";
    if(!strcmp(ext, "flv")) return "video/x-flv";
    if(!strcmp(ext, "3gp")) return "video/3gpp";
    /* Audio — needed for Cast/DLNA of MP3 (was falling through to octet-stream,
     * which the Cast receiver launches for but then can't play). */
    if(!strcmp(ext, "mp3")) return "audio/mpeg";
    if(!strcmp(ext, "m4a")) return "audio/mp4";
    if(!strcmp(ext, "aac")) return "audio/aac";
    if(!strcmp(ext, "wav")) return "audio/wav";
    if(!strcmp(ext, "flac")) return "audio/flac";
    if(!strcmp(ext, "ogg") || !strcmp(ext, "oga")) return "audio/ogg";
    return "application/octet-stream";
}

/* ---------------- send helpers ---------------- */

static bool send_all(int sock, const void* buf, int len) {
    const uint8_t* p = buf;
    int left = len;
    while(left > 0) {
        int n = lwip_send(sock, p, left, 0);
        if(n <= 0) return false;
        p += n;
        left -= n;
    }
    return true;
}

/* Serve the current file to `sock`. `head_only` for a HEAD request. Handles a
 * single "Range: bytes=start-[end]" request → 206, otherwise 200. */
/* Serve the file. range_start/range_end: an explicit "bytes=start-[end]" range
 * (both -1 if absent). range_suffix > 0: a "bytes=-N" suffix range (last N
 * bytes). 64-bit throughout so files up to the FAT32 4 GB limit are handled. */
static void serve_file(
    int sock, Storage* storage, bool head_only,
    int64_t range_start, int64_t range_end, int64_t range_suffix) {
    /* snapshot the served path under the mutex */
    char path[256], url_name[128];
    furi_mutex_acquire(s_file_mtx, FuriWaitForever);
    strncpy(path, s_file_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    strncpy(url_name, s_url_name, sizeof(url_name) - 1);
    url_name[sizeof(url_name) - 1] = '\0';
    furi_mutex_release(s_file_mtx);

    if(!path[0]) {
        const char* r = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(sock, r, strlen(r));
        return;
    }

    File* f = storage_file_alloc(storage);
    if(!storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(f);
        const char* r = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(sock, r, strlen(r));
        return;
    }

    int64_t total = (int64_t)storage_file_size(f);

    /* Explicit range starts past EOF → 416 (not a clamped 1-byte 206). A suffix
     * range larger than the file is fine per RFC (whole file), handled below. */
    if(range_start >= 0 && total > 0 && range_start >= total) {
        char r[128];
        int rn = snprintf(
            r, sizeof(r),
            "HTTP/1.1 416 Range Not Satisfiable\r\n"
            "Content-Range: bytes */%lld\r\n"
            "Connection: close\r\n\r\n",
            (long long)total);
        send_all(sock, r, rn);
        storage_file_close(f);
        storage_file_free(f);
        return;
    }

    int64_t start = 0;
    int64_t end = total - 1;
    bool partial = false;
    if(range_suffix > 0) {
        partial = true;
        start = total - range_suffix;
        if(start < 0) start = 0;
    } else if(range_start >= 0) {
        partial = true;
        start = range_start;
        if(range_end >= 0 && range_end < total) end = range_end;
    }
    if(start < 0) start = 0;
    if(total > 0 && start > total - 1) start = total - 1;
    int64_t clen = end - start + 1;
    if(clen < 0 || total == 0) clen = 0;

    const char* mime = dlna_render_mime_for(url_name);

    /* DLNA_ORG_OP=01 → byte-seek supported; flags = streaming + bg + connection
     * stall. This makes TVs enable the seek bar and buffer aggressively. */
    char hdr[512];
    int hn;
    if(partial) {
        hn = snprintf(
            hdr, sizeof(hdr),
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "Content-Range: bytes %lld-%lld/%lld\r\n"
            "Accept-Ranges: bytes\r\n"
            "transferMode.dlna.org: Streaming\r\n"
            "contentFeatures.dlna.org: DLNA.ORG_OP=01;DLNA.ORG_FLAGS=01700000000000000000000000000000\r\n"
            "Connection: close\r\n\r\n",
            mime, (long long)clen, (long long)start, (long long)end, (long long)total);
    } else {
        hn = snprintf(
            hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "Accept-Ranges: bytes\r\n"
            "transferMode.dlna.org: Streaming\r\n"
            "contentFeatures.dlna.org: DLNA.ORG_OP=01;DLNA.ORG_FLAGS=01700000000000000000000000000000\r\n"
            "Connection: close\r\n\r\n",
            mime, (long long)clen);
    }
    if(!send_all(sock, hdr, hn) || head_only || clen == 0) {
        storage_file_close(f);
        storage_file_free(f);
        return;
    }

    if(start > 0) storage_file_seek(f, (uint32_t)start, true);

    int64_t remaining = clen;
    while(remaining > 0 && s_httpd_run) {
        int64_t want = remaining > IO_CHUNK ? IO_CHUNK : remaining;
        uint16_t got = storage_file_read(f, s_io_buf, want > 0xFFFF ? 0xFFFF : (uint16_t)want);
        if(got == 0) break;
        if(!send_all(sock, s_io_buf, got)) break;
        remaining -= got;
    }

    storage_file_close(f);
    storage_file_free(f);
}

/* Parse a run of decimal digits into int64 (64-bit safe, unlike atol). Stops
 * well before signed overflow (UB); any real byte offset fits far below this. */
static int64_t parse_i64(const char* s) {
    int64_t v = 0;
    while(*s >= '0' && *s <= '9') {
        if(v > 0x0FFFFFFFFFFFFFFFLL) break;
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

/* Parse the request: method + "Range: bytes=start-end" or "bytes=-suffix". */
static void handle_conn(int sock, Storage* storage) {
    char req[REQ_BUF];
    int total = 0;
    /* read until end of headers or buffer full */
    while(total < REQ_BUF - 1) {
        int n = lwip_recv(sock, req + total, REQ_BUF - 1 - total, 0);
        if(n <= 0) break;
        total += n;
        req[total] = '\0';
        if(strstr(req, "\r\n\r\n")) break;
    }
    if(total <= 0) return;
    req[total] = '\0';

    bool head_only = (strncasecmp(req, "HEAD ", 5) == 0);
    bool is_get = (strncasecmp(req, "GET ", 4) == 0);
    if(!is_get && !head_only) {
        const char* r = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(sock, r, strlen(r));
        return;
    }

    int64_t rstart = -1, rend = -1, rsuffix = -1;
    /* case-insensitive search for the Range header */
    for(char* p = req; *p; p++) {
        if((p[0] == 'R' || p[0] == 'r') && strncasecmp(p, "Range:", 6) == 0) {
            char* b = strstr(p, "bytes=");
            if(b) {
                b += 6;
                while(*b == ' ') b++;
                if(*b == '-') {
                    /* suffix range: bytes=-N → last N bytes */
                    rsuffix = parse_i64(b + 1);
                } else {
                    rstart = parse_i64(b);
                    char* dash = strchr(b, '-');
                    if(dash && isdigit((unsigned char)dash[1])) rend = parse_i64(dash + 1);
                }
            }
            break;
        }
    }

    serve_file(sock, storage, head_only, rstart, rend, rsuffix);
}

static void httpd_task_fn(void* arg) {
    UNUSED(arg);
    ESP_LOGI(TAG, "httpd task start");

    Storage* storage = furi_record_open(RECORD_STORAGE);

    int ls = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(ls < 0) {
        ESP_LOGE(TAG, "listen socket failed");
        furi_record_close(RECORD_STORAGE);
        s_httpd_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    int one = 1;
    lwip_setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    /* Bind to INADDR_ANY, not the STA IP: the media URL already carries the STA
     * IP, and binding to ANY survives a WiFi reconnect that changes the IP. */
    addr.sin_addr.s_addr = 0;
    addr.sin_port = nbo16(DLNA_HTTP_PORT);
    if(lwip_bind(ls, (struct sockaddr*)&addr, sizeof(addr)) < 0 || lwip_listen(ls, 4) < 0) {
        ESP_LOGE(TAG, "bind/listen failed");
        lwip_close(ls);
        furi_record_close(RECORD_STORAGE);
        s_httpd_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_listen_sock = ls;

    /* accept timeout so the loop can poll s_httpd_run */
    struct timeval tv = {.tv_sec = 0, .tv_usec = 300 * 1000};
    lwip_setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while(s_httpd_run) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int cs = lwip_accept(ls, (struct sockaddr*)&cli, &cl);
        if(cs < 0) continue; /* timeout → re-check run flag */

        /* per-connection send/recv timeouts (bounded so a stalled TV can't pin
         * the task forever; stop() also shuts the socket down to unblock fast) */
        struct timeval ct = {.tv_sec = 6, .tv_usec = 0};
        lwip_setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, &ct, sizeof(ct));
        lwip_setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &ct, sizeof(ct));
        /* Disable Nagle so each video chunk goes out immediately instead of
         * waiting to coalesce — cuts stalls that make the TV re-buffer. */
        int one = 1;
        lwip_setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        s_client_sock = cs;
        handle_conn(cs, storage);
        s_client_sock = -1;
        lwip_close(cs);
    }

    lwip_close(ls);
    s_listen_sock = -1;
    furi_record_close(RECORD_STORAGE);
    ESP_LOGI(TAG, "httpd task exit");
    s_httpd_task = NULL;
    vTaskDelete(NULL);
}

bool dlna_render_httpd_start(uint32_t local_ip) {
    if(s_httpd_task) return true;
    if(!s_file_mtx) s_file_mtx = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!s_io_buf) s_io_buf = heap_caps_malloc(IO_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!s_io_buf) {
        ESP_LOGE(TAG, "io buf alloc failed");
        return false;
    }

    s_local_ip = local_ip;
    s_httpd_run = true;

    s_httpd_stack =
        heap_caps_malloc(HTTPD_STACK * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_httpd_buf = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!s_httpd_stack || !s_httpd_buf) {
        free(s_httpd_stack);
        s_httpd_stack = NULL;
        free(s_httpd_buf);
        s_httpd_buf = NULL;
        heap_caps_free(s_io_buf);
        s_io_buf = NULL;
        s_httpd_run = false;
        ESP_LOGE(TAG, "httpd stack/TCB alloc failed");
        return false;
    }

    s_httpd_task = xTaskCreateStaticPinnedToCore(
        httpd_task_fn, "DlnaHttpd", HTTPD_STACK, NULL, 5, s_httpd_stack, s_httpd_buf, 0);
    if(!s_httpd_task) {
        free(s_httpd_stack);
        s_httpd_stack = NULL;
        free(s_httpd_buf);
        s_httpd_buf = NULL;
        heap_caps_free(s_io_buf);
        s_io_buf = NULL;
        s_httpd_run = false;
        return false;
    }
    return true;
}

void dlna_render_httpd_stop(void) {
    s_httpd_run = false;
    /* Unblock a task stuck in accept()/send()/recv() immediately — a TV that
     * keeps its GET connection open (buffering/pause) would otherwise hold the
     * task inside lwip_send up to the 6 s socket timeout. */
    if(s_listen_sock >= 0) lwip_shutdown(s_listen_sock, SHUT_RDWR);
    if(s_client_sock >= 0) lwip_shutdown(s_client_sock, SHUT_RDWR);

    /* Wait up to ~8 s (> the 6 s socket timeout) for the task to self-delete. */
    for(int i = 0; i < 400 && s_httpd_task; i++) furi_delay_ms(20);

    if(s_httpd_task) {
        /* Task still running — freeing its stack/TCB/buffers now would be a
         * use-after-free (it executes FAP-text code). Leak them instead: safer
         * than a crash on the next FAP load. Should never happen in practice. */
        ESP_LOGE(TAG, "httpd task did not stop; leaking its buffers");
        return;
    }
    furi_delay_ms(20); /* let the scheduler reclaim the deleted task */

    if(s_httpd_stack) {
        free(s_httpd_stack);
        s_httpd_stack = NULL;
    }
    if(s_httpd_buf) {
        free(s_httpd_buf);
        s_httpd_buf = NULL;
    }
    if(s_io_buf) {
        heap_caps_free(s_io_buf);
        s_io_buf = NULL;
    }
    if(s_file_mtx) {
        furi_mutex_free(s_file_mtx);
        s_file_mtx = NULL;
    }
}

void dlna_render_set_file(const char* sd_path, const char* url_name) {
    if(!s_file_mtx) s_file_mtx = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_mutex_acquire(s_file_mtx, FuriWaitForever);
    strncpy(s_file_path, sd_path ? sd_path : "", sizeof(s_file_path) - 1);
    s_file_path[sizeof(s_file_path) - 1] = '\0';
    strncpy(s_url_name, url_name ? url_name : "", sizeof(s_url_name) - 1);
    s_url_name[sizeof(s_url_name) - 1] = '\0';
    furi_mutex_release(s_file_mtx);
}

/* Percent-encode everything but RFC 3986 unreserved chars + '/'. Without this a
 * file name with a space or umlaut produces an invalid request line on the TV
 * ("GET /My Video.mp4 HTTP/1.1"). The server ignores the path anyway (it always
 * serves the one set file), so we only need the URL to be syntactically valid. */
static void url_encode(const char* src, char* dst, int dst_sz) {
    static const char* hex = "0123456789ABCDEF";
    int j = 0;
    for(const unsigned char* p = (const unsigned char*)src; *p && j < dst_sz - 4; p++) {
        unsigned char c = *p;
        if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            dst[j++] = (char)c;
        } else {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0x0F];
        }
    }
    dst[j] = '\0';
}

void dlna_render_media_url(uint32_t local_ip, char* out, int out_sz) {
    char name[128];
    if(!s_file_mtx) s_file_mtx = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_mutex_acquire(s_file_mtx, FuriWaitForever);
    strncpy(name, s_url_name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    furi_mutex_release(s_file_mtx);

    char enc[3 * 128 + 1];
    url_encode(name, enc, sizeof(enc));

    snprintf(
        out, out_sz, "http://%u.%u.%u.%u:%u/%s",
        (unsigned)(local_ip & 0xff), (unsigned)((local_ip >> 8) & 0xff),
        (unsigned)((local_ip >> 16) & 0xff), (unsigned)((local_ip >> 24) & 0xff),
        (unsigned)DLNA_HTTP_PORT, enc);
}

/* ================= SOAP control ================= */

/* XML-escape src into dst (for embedding DIDL in CurrentURIMetaData / titles). */
static void xml_escape(const char* src, char* dst, int dst_sz) {
    int j = 0;
    for(const char* p = src; *p && j < dst_sz - 7; p++) {
        switch(*p) {
        case '<': memcpy(dst + j, "&lt;", 4); j += 4; break;
        case '>': memcpy(dst + j, "&gt;", 4); j += 4; break;
        case '&': memcpy(dst + j, "&amp;", 5); j += 5; break;
        case '"': memcpy(dst + j, "&quot;", 6); j += 6; break;
        default: dst[j++] = *p; break;
        }
    }
    dst[j] = '\0';
}

/* One blocking SOAP POST. `service` is "AVTransport" or "RenderingControl",
 * `action` the SOAP action, `inner` the body args after <InstanceID>0</...>.
 * Returns true on HTTP 200; copies the response body into resp (if non-NULL). */
static bool soap_call(
    const DlnaTarget* t, const char* control, const char* service,
    const char* action, const char* inner, char* resp, int resp_sz) {
    if(!control || !control[0]) return false;

    /* Big scratch off the stack: only the single WiFi worker task ever runs a
     * SOAP call (guarded by cmd_busy + the worker's serial command queue), so
     * static buffers are safe and keep the worker stack shallow. */
    static EXT_RAM_BSS_ATTR char body[5120];
    int bn = snprintf(
        body, sizeof(body),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>"
        "<u:%s xmlns:u=\"urn:schemas-upnp-org:service:%s:1\">"
        "<InstanceID>0</InstanceID>%s"
        "</u:%s>"
        "</s:Body></s:Envelope>",
        action, service, inner ? inner : "", action);
    /* snprintf returns the INTENDED length: a truncated body would make us send
     * past the buffer (OOB read) and a wrong Content-Length. Bail instead. */
    if(bn < 0 || bn >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "soap body too large (%d)", bn);
        return false;
    }

    int s = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(s < 0) return false;

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = nbo16(t->port);
    dst.sin_addr.s_addr = t->ip;

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    lwip_setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if(lwip_connect(s, (struct sockaddr*)&dst, sizeof(dst)) < 0) {
        lwip_close(s);
        return false;
    }

    char hdr[512];
    int hn = snprintf(
        hdr, sizeof(hdr),
        "POST %s HTTP/1.1\r\n"
        "HOST: %u.%u.%u.%u:%u\r\n"
        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
        "CONTENT-LENGTH: %d\r\n"
        "SOAPACTION: \"urn:schemas-upnp-org:service:%s:1#%s\"\r\n"
        "CONNECTION: close\r\n\r\n",
        control, (unsigned)(t->ip & 0xff), (unsigned)((t->ip >> 8) & 0xff),
        (unsigned)((t->ip >> 16) & 0xff), (unsigned)((t->ip >> 24) & 0xff),
        (unsigned)t->port, bn, service, action);

    if(!send_all(s, hdr, hn) || !send_all(s, body, bn)) {
        lwip_close(s);
        return false;
    }

    static EXT_RAM_BSS_ATTR char rbuf[SOAP_RESP_BUF];
    int total = 0;
    while(total < (int)sizeof(rbuf) - 1) {
        int n = lwip_recv(s, rbuf + total, sizeof(rbuf) - 1 - total, 0);
        if(n <= 0) break;
        total += n;
    }
    rbuf[total] = '\0';
    lwip_close(s);

    /* Check the status line only (not the body — a fault body could contain
     * "200"). Renderers reply "HTTP/1.1 200 OK" or occasionally "HTTP/1.0". */
    bool ok = (strncmp(rbuf, "HTTP/1.1 200", 12) == 0) ||
              (strncmp(rbuf, "HTTP/1.0 200", 12) == 0);
    if(resp && resp_sz > 0) {
        strncpy(resp, rbuf, resp_sz - 1);
        resp[resp_sz - 1] = '\0';
    }
    ESP_LOGI(TAG, "soap %s → %s", action, ok ? "200" : "err");
    return ok;
}

bool dlna_soap_set_and_play(const DlnaTarget* t, const char* media_url, const char* title) {
    /* Build DIDL-Lite metadata, then XML-escape it for CurrentURIMetaData.
     * Static (worker-only, serial) to keep the worker stack shallow. Sized
     * generously: the URL is %-encoded (name ×3) and the whole DIDL is XML-
     * escaped a SECOND time into CurrentURIMetaData (each &lt; → &amp;lt; …),
     * which roughly doubles it — a 96-char name must never truncate here. */
    static EXT_RAM_BSS_ATTR char didl[2048];
    static EXT_RAM_BSS_ATTR char title_esc[512];
    static EXT_RAM_BSS_ATTR char url_esc[512];
    xml_escape(title ? title : "Video", title_esc, sizeof(title_esc));
    xml_escape(media_url, url_esc, sizeof(url_esc));
    /* Advertise the real content type (matches the HTTP Content-Type header) —
     * strict renderers reject a protocolInfo MIME that differs from the stream. */
    const char* mime = dlna_render_mime_for(media_url);
    snprintf(
        didl, sizeof(didl),
        "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
        "<item id=\"0\" parentID=\"-1\" restricted=\"1\">"
        "<dc:title>%s</dc:title>"
        "<upnp:class>object.item.videoItem</upnp:class>"
        "<res protocolInfo=\"http-get:*:%s:DLNA.ORG_OP=01;"
        "DLNA.ORG_FLAGS=01700000000000000000000000000000\">%s</res>"
        "</item></DIDL-Lite>",
        title_esc, mime, url_esc);

    static EXT_RAM_BSS_ATTR char didl_esc[3072];
    xml_escape(didl, didl_esc, sizeof(didl_esc));

    static EXT_RAM_BSS_ATTR char inner[4096];
    snprintf(
        inner, sizeof(inner),
        "<CurrentURI>%s</CurrentURI><CurrentURIMetaData>%s</CurrentURIMetaData>",
        url_esc, didl_esc);

    if(!soap_call(t, t->av_control, "AVTransport", "SetAVTransportURI", inner, NULL, 0))
        return false;
    furi_delay_ms(250); /* give the renderer time to load the URI before Play */
    return dlna_soap_play(t);
}

bool dlna_soap_play(const DlnaTarget* t) {
    return soap_call(t, t->av_control, "AVTransport", "Play", "<Speed>1</Speed>", NULL, 0);
}

bool dlna_soap_pause(const DlnaTarget* t) {
    return soap_call(t, t->av_control, "AVTransport", "Pause", "", NULL, 0);
}

bool dlna_soap_stop(const DlnaTarget* t) {
    return soap_call(t, t->av_control, "AVTransport", "Stop", "", NULL, 0);
}

bool dlna_soap_seek(const DlnaTarget* t, uint32_t position_sec) {
    char tgt[64];
    unsigned h = position_sec / 3600;
    unsigned m = (position_sec % 3600) / 60;
    unsigned sec = position_sec % 60;
    char inner[96];
    snprintf(tgt, sizeof(tgt), "%u:%02u:%02u", h, m, sec);
    snprintf(inner, sizeof(inner), "<Unit>REL_TIME</Unit><Target>%s</Target>", tgt);
    return soap_call(t, t->av_control, "AVTransport", "Seek", inner, NULL, 0);
}

/* Parse "H:MM:SS" (or "HH:MM:SS") into milliseconds. */
static uint32_t hms_to_ms(const char* s) {
    unsigned h = 0, m = 0, sec = 0;
    if(sscanf(s, "%u:%u:%u", &h, &m, &sec) < 3) return 0;
    return ((h * 3600u) + (m * 60u) + sec) * 1000u;
}

bool dlna_soap_get_position(const DlnaTarget* t, uint32_t* elapsed_ms, uint32_t* duration_ms) {
    static EXT_RAM_BSS_ATTR char resp[SOAP_RESP_BUF];
    if(!soap_call(
           t, t->av_control, "AVTransport", "GetPositionInfo", "", resp, sizeof(resp)))
        return false;

    char dur[32] = {0}, rel[32] = {0};
    /* extract <TrackDuration>..</TrackDuration> and <RelTime>..</RelTime> */
    const char* a = strstr(resp, "<TrackDuration>");
    if(a) {
        a += 15;
        const char* b = strstr(a, "</TrackDuration>");
        if(b && (b - a) < (int)sizeof(dur)) {
            memcpy(dur, a, b - a);
            dur[b - a] = '\0';
        }
    }
    a = strstr(resp, "<RelTime>");
    if(a) {
        a += 9;
        const char* b = strstr(a, "</RelTime>");
        if(b && (b - a) < (int)sizeof(rel)) {
            memcpy(rel, a, b - a);
            rel[b - a] = '\0';
        }
    }
    if(duration_ms) *duration_ms = hms_to_ms(dur);
    if(elapsed_ms) *elapsed_ms = hms_to_ms(rel);
    return true;
}

bool dlna_soap_set_volume(const DlnaTarget* t, uint8_t volume) {
    if(!t->rc_control || !t->rc_control[0]) return false;
    char inner[96];
    snprintf(
        inner, sizeof(inner),
        "<Channel>Master</Channel><DesiredVolume>%u</DesiredVolume>", (unsigned)volume);
    return soap_call(t, t->rc_control, "RenderingControl", "SetVolume", inner, NULL, 0);
}
