#include "cast_client.h"
#include <esp_attr.h>

#include <lwip/sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <furi.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define TAG "Cast"

#define SESSION_STACK 8192 /* words; TLS handshake is deep */
#define RX_BUF        4096
#define TX_BUF        2048
#define PAYLOAD_BUF   1200

#define NS_CONNECTION "urn:x-cast:com.google.cast.tp.connection"
#define NS_HEARTBEAT  "urn:x-cast:com.google.cast.tp.heartbeat"
#define NS_RECEIVER   "urn:x-cast:com.google.cast.receiver"
#define NS_MEDIA      "urn:x-cast:com.google.cast.media"
#define MEDIA_RECEIVER_APP "CC1AD845"
#define SENDER_ID     "sender-0"
#define DEFAULT_DEST  "receiver-0"

/* ---- session state (single session task; statics are fine) ---- */
static mbedtls_net_context s_net;
static mbedtls_ssl_context s_ssl;
static mbedtls_ssl_config s_conf;
static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
static bool s_tls_live = false;

static TaskHandle_t s_task = NULL;
static StackType_t* s_stack = NULL;
static StaticTask_t* s_task_buf = NULL;
static volatile bool s_run = false;
static volatile CastState s_state = CastStateIdle;

static uint32_t s_ip = 0;
static uint16_t s_port = CAST_PORT;
static EXT_RAM_BSS_ATTR char s_url[512];
static EXT_RAM_BSS_ATTR char s_mime[48];
static EXT_RAM_BSS_ATTR char s_title[96];
static EXT_RAM_BSS_ATTR char s_transport[64]; /* app session transportId */
static int s_media_session = 0;
static int s_req_id = 0;

/* control flags set by the UI, consumed by the session task */
static volatile bool s_cmd_play = false;
static volatile bool s_cmd_pause = false;
static volatile bool s_cmd_seek = false;
static volatile uint32_t s_cmd_seek_sec = 0;
static volatile bool s_cmd_load = false; /* re-LOAD s_url */

/* progress from MEDIA_STATUS */
static volatile uint32_t s_elapsed_ms = 0;
static volatile uint32_t s_duration_ms = 0;

static uint8_t* s_rxbuf = NULL;

/* ---------------- hand-rolled protobuf (encode) ---------------- */
typedef struct {
    uint8_t* p;
    size_t len;
    size_t cap;
} Pb;
static void pb_byte(Pb* b, uint8_t v) {
    if(b->len < b->cap) b->p[b->len] = v;
    b->len++;
}
static void pb_varint(Pb* b, uint64_t v) {
    do {
        uint8_t x = v & 0x7f;
        v >>= 7;
        if(v) x |= 0x80;
        pb_byte(b, x);
    } while(v);
}
static void pb_tag(Pb* b, int field, int wt) {
    pb_varint(b, ((uint64_t)field << 3) | wt);
}
static void pb_uint(Pb* b, int field, uint64_t v) {
    pb_tag(b, field, 0);
    pb_varint(b, v);
}
static void pb_str(Pb* b, int field, const char* s) {
    size_t n = strlen(s);
    pb_tag(b, field, 2);
    pb_varint(b, n);
    for(size_t i = 0; i < n; i++) pb_byte(b, (uint8_t)s[i]);
}

/* ---------------- protobuf (decode) — top-level scan ---------------- */
static bool vread(const uint8_t** p, const uint8_t* end, uint64_t* out) {
    uint64_t v = 0;
    int shift = 0;
    while(*p < end) {
        uint8_t b = *(*p)++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if(!(b & 0x80)) {
            *out = v;
            return true;
        }
        shift += 7;
        if(shift > 63) return false;
    }
    return false;
}
/* Get the bytes of a wire-type-2 field. Returns false if not present. */
static bool pb_get_bytes(
    const uint8_t* buf, uint32_t len, uint32_t field, const uint8_t** out, uint32_t* outlen) {
    const uint8_t* p = buf;
    const uint8_t* end = buf + len;
    while(p < end) {
        uint64_t key;
        if(!vread(&p, end, &key)) return false;
        uint32_t f = (uint32_t)(key >> 3);
        uint32_t wt = (uint32_t)(key & 7);
        if(wt == 0) {
            uint64_t v;
            if(!vread(&p, end, &v)) return false;
        } else if(wt == 2) {
            uint64_t n;
            if(!vread(&p, end, &n)) return false;
            if((uint64_t)(end - p) < n) return false;
            if(f == field) {
                *out = p;
                *outlen = (uint32_t)n;
                return true;
            }
            p += n;
        } else if(wt == 5) {
            if(end - p < 4) return false;
            p += 4;
        } else if(wt == 1) {
            if(end - p < 8) return false;
            p += 8;
        } else {
            return false;
        }
    }
    return false;
}

/* ---------------- tiny JSON extractors ---------------- */
static bool json_str(const char* json, const char* key, char* out, size_t cap) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if(!p) return false;
    p += strlen(pat);
    while(*p == ' ' || *p == ':') p++;
    if(*p != '"') return false;
    p++;
    size_t i = 0;
    while(*p && *p != '"' && i < cap - 1) out[i++] = *p++;
    out[i] = '\0';
    return true;
}
/* integer part of a numeric JSON value (enough for seconds / ids) */
static bool json_int(const char* json, const char* key, uint32_t* out) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if(!p) return false;
    p += strlen(pat);
    while(*p == ' ' || *p == ':') p++;
    if(*p < '0' || *p > '9') return false;
    uint32_t v = 0;
    while(*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    *out = v;
    return true;
}

/* ---------------- TLS transport ---------------- */
static void cast_tls_close(void) {
    if(!s_tls_live) return;
    mbedtls_ssl_close_notify(&s_ssl);
    mbedtls_net_free(&s_net);
    mbedtls_ssl_free(&s_ssl);
    mbedtls_ssl_config_free(&s_conf);
    mbedtls_ctr_drbg_free(&s_drbg);
    mbedtls_entropy_free(&s_entropy);
    s_tls_live = false;
}

static bool cast_tls_open(void) {
    mbedtls_net_init(&s_net);
    mbedtls_ssl_init(&s_ssl);
    mbedtls_ssl_config_init(&s_conf);
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_drbg);
    s_tls_live = true; /* so the failure path frees via cast_tls_close */

    const char* pers = "flipper-cast";
    if(mbedtls_ctr_drbg_seed(
           &s_drbg, mbedtls_entropy_func, &s_entropy, (const unsigned char*)pers,
           strlen(pers)) != 0) {
        ESP_LOGE(TAG, "drbg seed failed");
        return false;
    }

    char host[16], portstr[8];
    snprintf(
        host, sizeof(host), "%u.%u.%u.%u", (unsigned)(s_ip & 0xff),
        (unsigned)((s_ip >> 8) & 0xff), (unsigned)((s_ip >> 16) & 0xff),
        (unsigned)((s_ip >> 24) & 0xff));
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)s_port);

    if(mbedtls_net_connect(&s_net, host, portstr, MBEDTLS_NET_PROTO_TCP) != 0) {
        ESP_LOGE(TAG, "tcp connect %s:%s failed", host, portstr);
        return false;
    }
    /* recv timeout so the session loop can poll flags/keepalive */
    struct timeval tv = {.tv_sec = 0, .tv_usec = 200 * 1000};
    lwip_setsockopt(s_net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if(mbedtls_ssl_config_defaults(
           &s_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
           MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        return false;
    /* The receiver presents a self-signed cert; Cast senders don't verify it. */
    mbedtls_ssl_conf_authmode(&s_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&s_conf, mbedtls_ctr_drbg_random, &s_drbg);

    if(mbedtls_ssl_setup(&s_ssl, &s_conf) != 0) return false;
    mbedtls_ssl_set_bio(&s_ssl, &s_net, mbedtls_net_send, mbedtls_net_recv, NULL);

    uint32_t start = furi_get_tick();
    int ret;
    while((ret = mbedtls_ssl_handshake(&s_ssl)) != 0) {
        if(ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if(!s_run || furi_get_tick() - start > furi_ms_to_ticks(10000)) return false;
            continue;
        }
        ESP_LOGE(TAG, "handshake -0x%04x", (unsigned)-ret);
        return false;
    }
    ESP_LOGI(TAG, "TLS up to %s:%s", host, portstr);
    return true;
}

static bool ssl_write_all(const uint8_t* buf, size_t len) {
    size_t off = 0;
    uint32_t start = furi_get_tick();
    while(off < len) {
        int r = mbedtls_ssl_write(&s_ssl, buf + off, len - off);
        if(r > 0) {
            off += r;
            continue;
        }
        if(r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if(!s_run || furi_get_tick() - start > furi_ms_to_ticks(5000)) return false;
            continue;
        }
        return false;
    }
    return true;
}

static bool ssl_read_exact(uint8_t* buf, size_t n) {
    size_t got = 0;
    uint32_t start = furi_get_tick();
    while(got < n) {
        int r = mbedtls_ssl_read(&s_ssl, buf + got, n - got);
        if(r > 0) {
            got += r;
            continue;
        }
        if(r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if(!s_run || furi_get_tick() - start > furi_ms_to_ticks(8000)) return false;
            continue;
        }
        return false; /* closed */
    }
    return true;
}

/* ---------------- CastMessage send / recv ---------------- */
static bool cast_send(const char* ns, const char* dst, const char* payload) {
    static EXT_RAM_BSS_ATTR uint8_t msg[TX_BUF];
    Pb b = {msg, 0, sizeof(msg)};
    pb_uint(&b, 1, 0); /* protocol_version = CASTV2_1_0 */
    pb_str(&b, 2, SENDER_ID); /* source_id */
    pb_str(&b, 3, dst); /* destination_id */
    pb_str(&b, 4, ns); /* namespace */
    pb_uint(&b, 5, 0); /* payload_type = STRING */
    pb_str(&b, 6, payload); /* payload_utf8 */
    if(b.len > sizeof(msg)) {
        ESP_LOGE(TAG, "tx msg too large (%u)", (unsigned)b.len);
        return false;
    }
    uint8_t hdr[4] = {
        (uint8_t)(b.len >> 24), (uint8_t)(b.len >> 16), (uint8_t)(b.len >> 8), (uint8_t)b.len};
    if(!ssl_write_all(hdr, 4)) return false;
    return ssl_write_all(msg, b.len);
}

/* Returns 1 = message in s_rxbuf (*outlen set), 0 = idle (timeout), -1 = error. */
static int cast_recv(uint32_t* outlen) {
    uint8_t hdr[4];
    /* first byte with the recv timeout distinguishes idle from a live message */
    int r = mbedtls_ssl_read(&s_ssl, hdr, 1);
    if(r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    if(r <= 0) return -1;
    if(!ssl_read_exact(hdr + 1, 3)) return -1;
    uint32_t len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                   ((uint32_t)hdr[2] << 8) | hdr[3];
    if(len == 0 || len > RX_BUF) return -1;
    if(!ssl_read_exact(s_rxbuf, len)) return -1;
    *outlen = len;
    return 1;
}

/* ---------------- payload builders ---------------- */
static bool send_connect(const char* dst) {
    return cast_send(NS_CONNECTION, dst, "{\"type\":\"CONNECT\"}");
}
static bool send_launch(void) {
    char pl[96];
    snprintf(
        pl, sizeof(pl), "{\"type\":\"LAUNCH\",\"appId\":\"%s\",\"requestId\":%d}",
        MEDIA_RECEIVER_APP, ++s_req_id);
    return cast_send(NS_RECEIVER, DEFAULT_DEST, pl);
}
static bool send_load(void) {
    static EXT_RAM_BSS_ATTR char pl[PAYLOAD_BUF];
    snprintf(
        pl, sizeof(pl),
        "{\"type\":\"LOAD\",\"requestId\":%d,\"autoplay\":true,\"currentTime\":0,"
        "\"media\":{\"contentId\":\"%s\",\"streamType\":\"BUFFERED\",\"contentType\":\"%s\","
        "\"metadata\":{\"metadataType\":0,\"title\":\"%s\"}}}",
        ++s_req_id, s_url, s_mime, s_title);
    return cast_send(NS_MEDIA, s_transport, pl);
}
static bool send_media_cmd(const char* type) {
    char pl[128];
    snprintf(
        pl, sizeof(pl), "{\"type\":\"%s\",\"requestId\":%d,\"mediaSessionId\":%d}", type,
        ++s_req_id, s_media_session);
    return cast_send(NS_MEDIA, s_transport, pl);
}
static bool send_seek(uint32_t sec) {
    char pl[160];
    snprintf(
        pl, sizeof(pl),
        "{\"type\":\"SEEK\",\"requestId\":%d,\"mediaSessionId\":%d,\"currentTime\":%u}",
        ++s_req_id, s_media_session, (unsigned)sec);
    return cast_send(NS_MEDIA, s_transport, pl);
}

/* ---------------- incoming message handling ---------------- */
static void handle_payload(const char* ns, const char* json) {
    if(strstr(json, "\"PING\"")) {
        cast_send(NS_HEARTBEAT, DEFAULT_DEST, "{\"type\":\"PONG\"}");
        return;
    }
    if(strcmp(ns, NS_RECEIVER) == 0 && strstr(json, "RECEIVER_STATUS")) {
        /* pull the app-session transportId (first application entry) */
        char tid[64];
        if(json_str(json, "transportId", tid, sizeof(tid))) {
            if(strcmp(tid, s_transport) != 0) {
                strncpy(s_transport, tid, sizeof(s_transport) - 1);
                s_transport[sizeof(s_transport) - 1] = '\0';
                ESP_LOGI(TAG, "transportId=%s", s_transport);
            }
        }
        return;
    }
    if(strcmp(ns, NS_MEDIA) == 0 && strstr(json, "MEDIA_STATUS")) {
        uint32_t v;
        if(json_int(json, "mediaSessionId", &v)) s_media_session = (int)v;
        if(json_int(json, "currentTime", &v)) s_elapsed_ms = v * 1000;
        if(json_int(json, "duration", &v) && v > 0) s_duration_ms = v * 1000;
        if(strstr(json, "\"PAUSED\""))
            s_state = CastStatePaused;
        else if(strstr(json, "\"PLAYING\"") || strstr(json, "\"BUFFERING\""))
            s_state = CastStatePlaying;
        return;
    }
}

/* Receive one message, log it, and dispatch it. Returns 1 (handled), 0 (idle),
 * -1 (connection error). Big payload buffer is static — one session task only. */
static int cast_pump(void) {
    uint32_t l;
    int r = cast_recv(&l);
    if(r != 1) return r;
    static EXT_RAM_BSS_ATTR char ns[80];
    static EXT_RAM_BSS_ATTR char pl[RX_BUF];
    const uint8_t* d;
    uint32_t dl;
    ns[0] = 0;
    if(pb_get_bytes(s_rxbuf, l, 4, &d, &dl)) {
        size_t n = dl < sizeof(ns) - 1 ? dl : sizeof(ns) - 1;
        memcpy(ns, d, n);
        ns[n] = 0;
    }
    pl[0] = 0;
    if(pb_get_bytes(s_rxbuf, l, 6, &d, &dl)) {
        size_t n = dl < sizeof(pl) - 1 ? dl : sizeof(pl) - 1;
        memcpy(pl, d, n);
        pl[n] = 0;
    }
    /* strip the namespace prefix in the log to keep it short. Debug level: this
     * fires every few seconds (MEDIA_STATUS/PONG) — raise the log level to see it. */
    const char* nss = strrchr(ns, '.');
    ESP_LOGD(TAG, "rx [%s] %.160s", nss ? nss + 1 : ns, pl);
    handle_payload(ns, pl);
    return 1;
}

/* ---------------- session task ---------------- */
static void cast_session_task(void* arg) {
    UNUSED(arg);
    s_state = CastStateConnecting;
    s_transport[0] = '\0';
    s_media_session = 0;

    if(!cast_tls_open()) {
        s_state = CastStateFailed;
        goto done;
    }
    /* base connection + launch the media receiver */
    bool c_ok = send_connect(DEFAULT_DEST);
    bool l_ok = send_launch();
    ESP_LOGI(TAG, "connect=%d launch=%d", c_ok, l_ok);
    if(!c_ok || !l_ok) {
        s_state = CastStateFailed;
        goto done;
    }

    /* wait for RECEIVER_STATUS to give us the app transportId (up to ~10 s) */
    uint32_t start = furi_get_tick();
    while(s_run && !s_transport[0]) {
        int r = cast_pump();
        if(r < 0) {
            ESP_LOGE(TAG, "recv error waiting for RECEIVER_STATUS");
            s_state = CastStateFailed;
            goto done;
        }
        if(furi_get_tick() - start > furi_ms_to_ticks(10000)) {
            ESP_LOGE(TAG, "no transportId (launch failed?)");
            s_state = CastStateFailed;
            goto done;
        }
    }
    if(!s_run) goto done;

    /* connect to the app session + LOAD the media */
    bool ac_ok = send_connect(s_transport);
    bool ld_ok = send_load();
    ESP_LOGI(TAG, "app-connect=%d load=%d dst=%s", ac_ok, ld_ok, s_transport);
    if(!ac_ok || !ld_ok) {
        s_state = CastStateFailed;
        goto done;
    }
    s_state = CastStatePlaying;

    /* session loop: pump messages, answer pings, run UI commands */
    uint32_t last_ping = furi_get_tick();
    while(s_run) {
        int r = cast_pump();
        if(r < 0) {
            ESP_LOGW(TAG, "connection closed");
            break;
        }

        /* UI commands */
        if(s_cmd_load) {
            s_cmd_load = false;
            send_load();
        }
        if(s_cmd_pause) {
            s_cmd_pause = false;
            if(send_media_cmd("PAUSE")) s_state = CastStatePaused;
        }
        if(s_cmd_play) {
            s_cmd_play = false;
            if(send_media_cmd("PLAY")) s_state = CastStatePlaying;
        }
        if(s_cmd_seek) {
            s_cmd_seek = false;
            send_seek(s_cmd_seek_sec);
        }

        /* heartbeat every ~4.5 s + poll media status */
        if(furi_get_tick() - last_ping > furi_ms_to_ticks(4500)) {
            cast_send(NS_HEARTBEAT, DEFAULT_DEST, "{\"type\":\"PING\"}");
            if(s_transport[0]) send_media_cmd("GET_STATUS");
            last_ping = furi_get_tick();
        }
    }

done:
    /* best-effort STOP so the TV drops back to its home screen */
    if(s_tls_live && s_transport[0] && s_media_session)
        send_media_cmd("STOP");
    cast_tls_close();
    if(s_state != CastStateFailed) s_state = CastStateIdle;
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---------------- public API ---------------- */
bool cast_start(uint32_t ip, uint16_t port, const char* media_url, const char* mime, const char* title) {
    if(s_task) return false;
    if(!s_rxbuf) s_rxbuf = heap_caps_malloc(RX_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!s_rxbuf) return false;

    s_ip = ip;
    s_port = port ? port : CAST_PORT;
    strncpy(s_url, media_url ? media_url : "", sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    strncpy(s_mime, mime ? mime : "video/mp4", sizeof(s_mime) - 1);
    s_mime[sizeof(s_mime) - 1] = '\0';
    strncpy(s_title, title ? title : "Video", sizeof(s_title) - 1);
    s_title[sizeof(s_title) - 1] = '\0';
    s_req_id = 0;
    s_elapsed_ms = 0;
    s_duration_ms = 0;
    s_cmd_play = s_cmd_pause = s_cmd_seek = s_cmd_load = false;
    s_run = true;
    /* Publish "connecting" synchronously so the first UI tick after cast_start()
     * doesn't observe the stale Idle state (from cast_stop) and bounce the
     * player back to a "Play" prompt - the session task sets it again anyway. */
    s_state = CastStateConnecting;

    s_stack = heap_caps_malloc(SESSION_STACK * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_task_buf = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!s_stack || !s_task_buf) {
        free(s_stack);
        s_stack = NULL;
        free(s_task_buf);
        s_task_buf = NULL;
        s_run = false;
        ESP_LOGE(TAG, "session stack alloc failed");
        return false;
    }
    s_task = xTaskCreateStaticPinnedToCore(
        cast_session_task, "CastSession", SESSION_STACK, NULL, 5, s_stack, s_task_buf, 0);
    return s_task != NULL;
}

bool cast_load(const char* media_url, const char* mime, const char* title) {
    if(!s_task) return false;
    strncpy(s_url, media_url ? media_url : "", sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    strncpy(s_mime, mime ? mime : "video/mp4", sizeof(s_mime) - 1);
    s_mime[sizeof(s_mime) - 1] = '\0';
    strncpy(s_title, title ? title : "Video", sizeof(s_title) - 1);
    s_title[sizeof(s_title) - 1] = '\0';
    s_cmd_load = true;
    return true;
}

void cast_stop(void) {
    if(s_task) {
        s_run = false;
        for(int i = 0; i < 300 && s_task; i++) furi_delay_ms(20);
        furi_delay_ms(20);
    }
    if(s_stack) {
        free(s_stack);
        s_stack = NULL;
    }
    if(s_task_buf) {
        free(s_task_buf);
        s_task_buf = NULL;
    }
    if(s_rxbuf) {
        heap_caps_free(s_rxbuf);
        s_rxbuf = NULL;
    }
    s_state = CastStateIdle;
}

CastState cast_state(void) {
    return s_state;
}
void cast_ctrl_play(void) {
    s_cmd_play = true;
}
void cast_ctrl_pause(void) {
    s_cmd_pause = true;
}
void cast_ctrl_seek(uint32_t position_sec) {
    s_cmd_seek_sec = position_sec;
    s_cmd_seek = true;
}
void cast_get_progress(uint32_t* elapsed_ms, uint32_t* duration_ms) {
    if(elapsed_ms) *elapsed_ms = s_elapsed_ms;
    if(duration_ms) *duration_ms = s_duration_ms;
}
