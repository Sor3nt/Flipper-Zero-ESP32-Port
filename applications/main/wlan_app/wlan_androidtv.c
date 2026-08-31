#include "wlan_androidtv.h"

#include <furi.h>
#include <storage/storage.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <lwip/sockets.h> // struct timeval, setsockopt, SO_RCVTIMEO

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/bignum.h>
#include <mbedtls/sha256.h>

#include <string.h>
#include <stdlib.h>

#define TAG "WlanAtv"

// The Android TV Remote v2 protocol lives on two TLS ports.
#define ATV_PAIR_PORT "6467"
#define ATV_REMOTE_PORT "6466"

// The worker stack MUST be internal DRAM: it runs concurrently with WiFi,
// which briefly disables the flash cache for internal ops. A PSRAM stack would
// be unreachable during those windows -> "Cache disabled but cached memory
// region accessed" panic. Same reasoning as wlan_smb / wlan_hal. mbedTLS's own
// heap buffers go to PSRAM (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC) — that path is
// proven over WiFi by the HTTPS firmware-update client. TLS handshake needs
// ~8K stack (see the FW worker at 8192); RSA-2048 keygen adds headroom.
#define ATV_WORKER_STACK 20480
#define ATV_MSG_MAX 1024 // framed protobuf message body cap (messages are tiny)
#define ATV_PEM_MAX 2048 // PEM buffer for cert/key generation

// Client-cert persistence (shared across all TVs — the TV remembers our cert).
#define ATV_DIR "/ext/wifi/androidtv"
#define ATV_CERT_PATH ATV_DIR "/cert.pem"
#define ATV_KEY_PATH ATV_DIR "/key.pem"

// Client name shown on the TV during pairing (cosmetic; not in the hash).
#define ATV_CLIENT_NAME "Flipper32"

// Feature mask we advertise back in remote_configure/set_active:
// PING(1)|KEY(2)|POWER(32)|VOLUME(64)|APP_LINK(512) = 611.
#define ATV_FEATURES 611

typedef enum {
    AtvCmdNone = 0,
    AtvCmdConnect, // open remote-control session (long-lived)
    AtvCmdPairStart, // begin pairing (waits for a PIN, then finishes)
    AtvCmdQuit,
} AtvCmdType;

struct WlanAndroidTv {
    // Worker task + command channel. The TCB must live in internal DRAM
    // (FreeRTOS asserts on it), hence a separate internal-DRAM pointer.
    TaskHandle_t task;
    StaticTask_t* task_buf;
    StackType_t* stack;
    QueueHandle_t cmd_queue; // AtvCmdType
    QueueHandle_t key_queue; // int keycodes (drained by the session loop)

    volatile WlanAtvState state;
    volatile bool cancel; // abort the current op / session loop
    volatile bool session_alive;
    char error[WLAN_ATV_ERR_MAX];
    char device_name[WLAN_ATV_NAME_MAX];

    char ip[64]; // pending op target
    char pin[16]; // set by pair_finish
    volatile bool pin_ready;

    // mbedTLS state (persisted so the cert is loaded once).
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509_crt client_crt;
    mbedtls_pk_context client_key;
    bool crt_loaded;

    // Live connection (valid between tls_open and tls_close).
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    bool tls_live;

    uint8_t* rxbuf; // PSRAM
    uint8_t* txbuf; // PSRAM
    Storage* storage;
};

// ===========================================================================
// Small helpers
// ===========================================================================
static void atv_set_error(WlanAndroidTv* a, const char* msg) {
    strncpy(a->error, msg ? msg : "unknown error", sizeof(a->error) - 1);
    a->error[sizeof(a->error) - 1] = '\0';
    ESP_LOGW(TAG, "%s", a->error);
}

static bool atv_deadline_passed(uint32_t start_tick, uint32_t ms) {
    return (furi_get_tick() - start_tick) > furi_ms_to_ticks(ms);
}

// ===========================================================================
// Hand-rolled protobuf (encode) — verified byte-for-byte against the
// androidtvremote2 wire output.
// ===========================================================================
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
static void pb_bytes(Pb* b, int field, const uint8_t* d, size_t n) {
    pb_tag(b, field, 2);
    pb_varint(b, n);
    for(size_t i = 0; i < n; i++) pb_byte(b, d[i]);
}
static void pb_str(Pb* b, int field, const char* s) {
    pb_bytes(b, field, (const uint8_t*)s, strlen(s));
}

// ===========================================================================
// protobuf (decode) — top-level field scanner
// ===========================================================================
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

// Decode one field. On success advances *p and fills field/wt plus either
// *vint (wt 0) or *data/*dlen (wt 2). Other wire types are skipped.
static bool pb_next(
    const uint8_t** p,
    const uint8_t* end,
    uint32_t* field,
    uint32_t* wt,
    uint64_t* vint,
    const uint8_t** data,
    uint32_t* dlen) {
    if(*p >= end) return false;
    uint64_t key;
    if(!vread(p, end, &key)) return false;
    *field = (uint32_t)(key >> 3);
    *wt = (uint32_t)(key & 7);
    *vint = 0;
    *data = NULL;
    *dlen = 0;
    if(*wt == 0) {
        if(!vread(p, end, vint)) return false;
    } else if(*wt == 2) {
        uint64_t n;
        if(!vread(p, end, &n)) return false;
        if((uint64_t)(end - *p) < n) return false;
        *data = *p;
        *dlen = (uint32_t)n;
        *p += n;
    } else if(*wt == 5) {
        if(end - *p < 4) return false;
        *p += 4;
    } else if(*wt == 1) {
        if(end - *p < 8) return false;
        *p += 8;
    } else {
        return false;
    }
    return true;
}

static bool pb_has(const uint8_t* buf, uint32_t len, uint32_t field) {
    const uint8_t* p = buf;
    const uint8_t* end = buf + len;
    uint32_t f, wt, dl;
    uint64_t v;
    const uint8_t* d;
    while(pb_next(&p, end, &f, &wt, &v, &d, &dl)) {
        if(f == field) return true;
    }
    return false;
}

static bool pb_get_uint(const uint8_t* buf, uint32_t len, uint32_t field, uint64_t* out) {
    const uint8_t* p = buf;
    const uint8_t* end = buf + len;
    uint32_t f, wt, dl;
    uint64_t v;
    const uint8_t* d;
    while(pb_next(&p, end, &f, &wt, &v, &d, &dl)) {
        if(f == field && wt == 0) {
            *out = v;
            return true;
        }
    }
    return false;
}

static bool pb_get_bytes(
    const uint8_t* buf,
    uint32_t len,
    uint32_t field,
    const uint8_t** out,
    uint32_t* outlen) {
    const uint8_t* p = buf;
    const uint8_t* end = buf + len;
    uint32_t f, wt, dl;
    uint64_t v;
    const uint8_t* d;
    while(pb_next(&p, end, &f, &wt, &v, &d, &dl)) {
        if(f == field && wt == 2) {
            *out = d;
            *outlen = dl;
            return true;
        }
    }
    return false;
}

// ===========================================================================
// TLS transport
// ===========================================================================
static void atv_tls_close(WlanAndroidTv* a) {
    if(!a->tls_live) return;
    mbedtls_ssl_close_notify(&a->ssl);
    mbedtls_net_free(&a->net);
    mbedtls_ssl_free(&a->ssl);
    mbedtls_ssl_config_free(&a->conf);
    a->tls_live = false;
}

// Returns: 0 ok, -1 TCP connect failed, -2 TLS handshake failed.
static int atv_tls_open(WlanAndroidTv* a, const char* port) {
    mbedtls_net_init(&a->net);
    mbedtls_ssl_init(&a->ssl);
    mbedtls_ssl_config_init(&a->conf);
    a->tls_live = true; // so a failure path still frees via atv_tls_close

    if(mbedtls_net_connect(&a->net, a->ip, port, MBEDTLS_NET_PROTO_TCP) != 0) {
        return -1;
    }
    // 150 ms recv timeout -> the session loop wakes to poll keys/cancel.
    struct timeval tv = {.tv_sec = 0, .tv_usec = 150 * 1000};
    setsockopt(a->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if(mbedtls_ssl_config_defaults(
           &a->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
           MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return -2;
    }
    // The TV presents a self-signed cert we cannot verify (TOFU pairing); the
    // trust anchor is our own client cert, remembered by the TV.
    mbedtls_ssl_conf_authmode(&a->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&a->conf, mbedtls_ctr_drbg_random, &a->ctr_drbg);
    mbedtls_ssl_conf_own_cert(&a->conf, &a->client_crt, &a->client_key);
    // Android TV remote speaks TLS 1.2.
    mbedtls_ssl_conf_min_tls_version(&a->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&a->conf, MBEDTLS_SSL_VERSION_TLS1_2);

    if(mbedtls_ssl_setup(&a->ssl, &a->conf) != 0) return -2;
    mbedtls_ssl_set_bio(&a->ssl, &a->net, mbedtls_net_send, mbedtls_net_recv, NULL);

    uint32_t start = furi_get_tick();
    int ret;
    while((ret = mbedtls_ssl_handshake(&a->ssl)) != 0) {
        if(ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if(a->cancel || atv_deadline_passed(start, 10000)) return -2;
            continue;
        }
        ESP_LOGW(TAG, "handshake failed -0x%04x", (unsigned)-ret);
        return -2;
    }
    return 0;
}

static int atv_ssl_write_all(WlanAndroidTv* a, const uint8_t* buf, size_t len) {
    size_t off = 0;
    uint32_t start = furi_get_tick();
    while(off < len) {
        int r = mbedtls_ssl_write(&a->ssl, buf + off, len - off);
        if(r > 0) {
            off += r;
            continue;
        }
        if(r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if(a->cancel || atv_deadline_passed(start, 5000)) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

// Send a length-prefixed (varint) protobuf message.
static int atv_send_msg(WlanAndroidTv* a, const uint8_t* body, size_t len) {
    uint8_t hdr[5];
    Pb h = {hdr, 0, sizeof(hdr)};
    pb_varint(&h, len);
    if(atv_ssl_write_all(a, hdr, h.len) < 0) return -1;
    return atv_ssl_write_all(a, body, len);
}

// Read exactly n bytes with WANT_READ retry (used mid-message). Returns 0/-1.
static int atv_ssl_read_exact(WlanAndroidTv* a, uint8_t* buf, size_t n) {
    size_t got = 0;
    uint32_t start = furi_get_tick();
    while(got < n) {
        int r = mbedtls_ssl_read(&a->ssl, buf + got, n - got);
        if(r > 0) {
            got += r;
            continue;
        }
        if(r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if(a->cancel || atv_deadline_passed(start, 8000)) return -1;
            continue;
        }
        return -1; // closed / error
    }
    return 0;
}

// Read one framed message. Returns 1 (got, *outlen set), 0 (idle: no byte
// arrived within the recv timeout), or -1 (closed/error).
static int atv_recv_msg(WlanAndroidTv* a, uint32_t* outlen) {
    uint8_t b;
    int r = mbedtls_ssl_read(&a->ssl, &b, 1);
    if(r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    if(r <= 0) return -1;

    // Decode the varint length prefix (b is its first byte).
    uint32_t len = b & 0x7f;
    int shift = 7;
    while(b & 0x80) {
        if(atv_ssl_read_exact(a, &b, 1) < 0) return -1;
        len |= (uint32_t)(b & 0x7f) << shift;
        shift += 7;
        if(shift > 28) return -1;
    }
    if(len > ATV_MSG_MAX) return -1;
    if(len && atv_ssl_read_exact(a, a->rxbuf, len) < 0) return -1;
    *outlen = len;
    return 1;
}

// Block until a message arrives or the deadline/cancel hits. Returns 1/-1.
static int atv_recv_msg_blocking(WlanAndroidTv* a, uint32_t* outlen, uint32_t deadline_ms) {
    uint32_t start = furi_get_tick();
    for(;;) {
        int r = atv_recv_msg(a, outlen);
        if(r != 0) return r;
        if(a->cancel || atv_deadline_passed(start, deadline_ms)) return -1;
    }
}

// ===========================================================================
// Certificate: load / generate / persist
// ===========================================================================
static bool atv_read_file(WlanAndroidTv* a, const char* path, uint8_t* buf, size_t cap, size_t* out) {
    File* f = storage_file_alloc(a->storage);
    bool ok = false;
    if(storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint16_t n = storage_file_read(f, buf, cap - 1);
        buf[n] = '\0';
        *out = n;
        ok = n > 0;
        storage_file_close(f);
    }
    storage_file_free(f);
    return ok;
}

static bool atv_write_file(WlanAndroidTv* a, const char* path, const uint8_t* buf, size_t len) {
    File* f = storage_file_alloc(a->storage);
    bool ok = false;
    if(storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        ok = (storage_file_write(f, buf, len) == len);
        storage_file_close(f);
    }
    storage_file_free(f);
    return ok;
}

static bool atv_cert_load(WlanAndroidTv* a) {
    if(a->crt_loaded) return true;

    uint8_t* cbuf = heap_caps_malloc(ATV_PEM_MAX, MALLOC_CAP_SPIRAM);
    uint8_t* kbuf = heap_caps_malloc(ATV_PEM_MAX, MALLOC_CAP_SPIRAM);
    bool ok = false;
    size_t clen = 0, klen = 0;
    if(cbuf && kbuf && atv_read_file(a, ATV_CERT_PATH, cbuf, ATV_PEM_MAX, &clen) &&
       atv_read_file(a, ATV_KEY_PATH, kbuf, ATV_PEM_MAX, &klen)) {
        // PEM parse needs the length to include the trailing NUL.
        if(mbedtls_x509_crt_parse(&a->client_crt, cbuf, clen + 1) == 0 &&
           mbedtls_pk_parse_key(
               &a->client_key, kbuf, klen + 1, NULL, 0, mbedtls_ctr_drbg_random, &a->ctr_drbg) ==
               0) {
            a->crt_loaded = true;
            ok = true;
        } else {
            ESP_LOGW(TAG, "stored cert/key parse failed");
        }
    }
    if(cbuf) free(cbuf);
    if(kbuf) free(kbuf);
    return ok;
}

// Generate a fresh self-signed RSA-2048 client cert and persist it. Slow
// (a few seconds) but runs only once. Returns true and leaves the cert loaded.
static bool atv_cert_generate(WlanAndroidTv* a) {
    ESP_LOGI(TAG, "generating client certificate (RSA-2048)...");
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    uint8_t* cert_pem = heap_caps_malloc(ATV_PEM_MAX, MALLOC_CAP_SPIRAM);
    uint8_t* key_pem = heap_caps_malloc(ATV_PEM_MAX, MALLOC_CAP_SPIRAM);
    bool ok = false;

    do {
        if(!cert_pem || !key_pem) break;
        if(mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0) break;
        if(mbedtls_rsa_gen_key(
               mbedtls_pk_rsa(key), mbedtls_ctr_drbg_random, &a->ctr_drbg, 2048, 65537) != 0)
            break;

        mbedtls_x509write_crt_set_subject_key(&crt, &key);
        mbedtls_x509write_crt_set_issuer_key(&crt, &key);
        if(mbedtls_x509write_crt_set_subject_name(&crt, "CN=atvremote") != 0) break;
        if(mbedtls_x509write_crt_set_issuer_name(&crt, "CN=atvremote") != 0) break;
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        unsigned char serial[] = {0x01};
        mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
        if(mbedtls_x509write_crt_set_validity(&crt, "20200101000000", "20350101000000") != 0)
            break;
        mbedtls_x509write_crt_set_basic_constraints(&crt, 1, 0);

        if(mbedtls_x509write_crt_pem(
               &crt, cert_pem, ATV_PEM_MAX, mbedtls_ctr_drbg_random, &a->ctr_drbg) != 0)
            break;
        if(mbedtls_pk_write_key_pem(&key, key_pem, ATV_PEM_MAX) != 0) break;

        storage_common_mkdir(a->storage, "/ext/wifi");
        storage_common_mkdir(a->storage, ATV_DIR);
        if(!atv_write_file(a, ATV_CERT_PATH, cert_pem, strlen((char*)cert_pem))) break;
        if(!atv_write_file(a, ATV_KEY_PATH, key_pem, strlen((char*)key_pem))) break;

        // Parse the freshly generated PEM into our live contexts.
        if(mbedtls_x509_crt_parse(&a->client_crt, cert_pem, strlen((char*)cert_pem) + 1) != 0)
            break;
        if(mbedtls_pk_parse_key(
               &a->client_key, key_pem, strlen((char*)key_pem) + 1, NULL, 0,
               mbedtls_ctr_drbg_random, &a->ctr_drbg) != 0)
            break;
        a->crt_loaded = true;
        ok = true;
        ESP_LOGI(TAG, "client certificate generated + saved");
    } while(0);

    mbedtls_pk_free(&key);
    mbedtls_x509write_crt_free(&crt);
    if(cert_pem) free(cert_pem);
    if(key_pem) free(key_pem);
    return ok;
}

// ===========================================================================
// Pairing (Polo protocol, port 6467)
// ===========================================================================
static int hexval(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Extract minimal big-endian modulus (N) and exponent (E) from an RSA pubkey.
static int atv_rsa_pub_parts(
    const mbedtls_pk_context* pk,
    uint8_t* nbuf,
    size_t* nlen,
    uint8_t* ebuf,
    size_t* elen) {
    mbedtls_rsa_context* rsa = mbedtls_pk_rsa(*pk);
    if(!rsa) return -1;
    mbedtls_mpi N, E;
    mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&E);
    int rc = mbedtls_rsa_export(rsa, &N, NULL, NULL, NULL, &E);
    if(rc == 0) {
        *nlen = mbedtls_mpi_size(&N);
        *elen = mbedtls_mpi_size(&E);
        if(mbedtls_mpi_write_binary(&N, nbuf, *nlen) != 0 ||
           mbedtls_mpi_write_binary(&E, ebuf, *elen) != 0)
            rc = -1;
    }
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&E);
    return rc;
}

// Send an OuterMessage carrying the length-delimited submessage `sub`.
static int atv_send_outer(WlanAndroidTv* a, int field, const uint8_t* sub, size_t sublen) {
    Pb m = {a->txbuf, 0, ATV_MSG_MAX};
    pb_uint(&m, 1, 2); // protocol_version = 2
    pb_uint(&m, 2, 200); // status = STATUS_OK
    pb_bytes(&m, field, sub, sublen);
    if(m.len > ATV_MSG_MAX) return -1;
    return atv_send_msg(a, a->txbuf, m.len);
}

// Phase 1: pairing_request -> ...ack/options/configuration... -> config_ack.
// On success the TV shows the PIN and we return 0 (connection kept open).
static int atv_pair_phase1(WlanAndroidTv* a) {
    uint8_t sbuf[128];

    { // pairing_request { service_name="atvremote", client_name=... }
        Pb s = {sbuf, 0, sizeof(sbuf)};
        pb_str(&s, 1, "atvremote");
        pb_str(&s, 2, ATV_CLIENT_NAME);
        if(atv_send_outer(a, 10, s.p, s.len) < 0) return -1;
    }

    uint32_t olen;
    for(;;) {
        if(atv_recv_msg_blocking(a, &olen, 10000) < 0) return -1;
        uint64_t status = 200;
        if(pb_get_uint(a->rxbuf, olen, 2, &status) && status != 200) {
            atv_set_error(a, "TV rejected pairing");
            return -1;
        }
        if(pb_has(a->rxbuf, olen, 11)) { // pairing_request_ack -> options
            Pb enc = {sbuf, 0, sizeof(sbuf)};
            pb_uint(&enc, 1, 3); // type = HEXADECIMAL
            pb_uint(&enc, 2, 6); // symbol_length = 6
            uint8_t obuf[64];
            Pb opt = {obuf, 0, sizeof(obuf)};
            pb_bytes(&opt, 1, enc.p, enc.len); // input_encodings
            pb_uint(&opt, 3, 1); // preferred_role = INPUT
            if(atv_send_outer(a, 20, opt.p, opt.len) < 0) return -1;
        } else if(pb_has(a->rxbuf, olen, 20)) { // options -> configuration
            Pb enc = {sbuf, 0, sizeof(sbuf)};
            pb_uint(&enc, 1, 3);
            pb_uint(&enc, 2, 6);
            uint8_t cbuf[64];
            Pb cfg = {cbuf, 0, sizeof(cbuf)};
            pb_bytes(&cfg, 1, enc.p, enc.len); // encoding
            pb_uint(&cfg, 2, 1); // client_role = INPUT
            if(atv_send_outer(a, 30, cfg.p, cfg.len) < 0) return -1;
        } else if(pb_has(a->rxbuf, olen, 31)) { // configuration_ack -> PIN shown
            return 0;
        }
    }
}

// Phase 2: compute the secret from client+server pubkeys and the PIN, send it,
// wait for secret_ack. Returns 0 (paired), -1 (error), -2 (wrong PIN).
static int atv_pair_phase2(WlanAndroidTv* a, const char* pin) {
    if(strlen(pin) != WLAN_ATV_PIN_LEN) {
        atv_set_error(a, "PIN must be 6 hex chars");
        return -2;
    }
    int c0 = hexval(pin[0]), c1 = hexval(pin[1]);
    int n0 = hexval(pin[2]), n1 = hexval(pin[3]);
    int n2 = hexval(pin[4]), n3 = hexval(pin[5]);
    if((c0 | c1 | n0 | n1 | n2 | n3) < 0) {
        atv_set_error(a, "PIN must be hex");
        return -2;
    }
    uint8_t check = (uint8_t)((c0 << 4) | c1);
    uint8_t nonce[2] = {(uint8_t)((n0 << 4) | n1), (uint8_t)((n2 << 4) | n3)};

    const mbedtls_x509_crt* peer = mbedtls_ssl_get_peer_cert(&a->ssl);
    if(!peer) {
        atv_set_error(a, "no server certificate");
        return -1;
    }

    static uint8_t cn[300], ce[16], sn[300], se[16];
    size_t cnl, cel, snl, sel;
    if(atv_rsa_pub_parts(&a->client_crt.pk, cn, &cnl, ce, &cel) != 0 ||
       atv_rsa_pub_parts(&peer->pk, sn, &snl, se, &sel) != 0) {
        atv_set_error(a, "RSA key export failed");
        return -1;
    }

    uint8_t digest[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, cn, cnl);
    mbedtls_sha256_update(&sha, ce, cel);
    mbedtls_sha256_update(&sha, sn, snl);
    mbedtls_sha256_update(&sha, se, sel);
    mbedtls_sha256_update(&sha, nonce, sizeof(nonce));
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    if(digest[0] != check) {
        atv_set_error(a, "Wrong PIN");
        return -2;
    }

    uint8_t sbuf[64];
    Pb s = {sbuf, 0, sizeof(sbuf)};
    pb_bytes(&s, 1, digest, sizeof(digest)); // Secret.secret
    if(atv_send_outer(a, 40, s.p, s.len) < 0) return -1;

    uint32_t olen;
    if(atv_recv_msg_blocking(a, &olen, 10000) < 0) {
        atv_set_error(a, "no secret_ack");
        return -1;
    }
    uint64_t status = 200;
    if(pb_get_uint(a->rxbuf, olen, 2, &status) && status != 200) {
        atv_set_error(a, "Wrong PIN");
        return -2;
    }
    if(pb_has(a->rxbuf, olen, 41)) return 0; // secret_ack
    atv_set_error(a, "pairing not acknowledged");
    return -1;
}

static void atv_do_pair(WlanAndroidTv* a) {
    a->state = WlanAtvStateBusy;
    a->error[0] = '\0';

    if(!atv_cert_load(a) && !atv_cert_generate(a)) {
        atv_set_error(a, "Cert generation failed");
        a->state = WlanAtvStateError;
        return;
    }

    int rc = atv_tls_open(a, ATV_PAIR_PORT);
    if(rc == -1) {
        atv_set_error(a, "TV unreachable");
        atv_tls_close(a);
        a->state = WlanAtvStateError;
        return;
    }
    if(rc == -2) {
        atv_set_error(a, "TLS handshake failed");
        atv_tls_close(a);
        a->state = WlanAtvStateError;
        return;
    }

    if(atv_pair_phase1(a) < 0) {
        if(!a->error[0]) atv_set_error(a, "Pairing setup failed");
        atv_tls_close(a);
        a->state = WlanAtvStateError;
        return;
    }

    // PIN is on the TV screen — wait for the user (pair_finish sets pin_ready).
    a->pin_ready = false;
    a->state = WlanAtvStatePairShowPin;
    uint32_t start = furi_get_tick();
    while(!a->pin_ready) {
        if(a->cancel || atv_deadline_passed(start, 120000)) {
            atv_tls_close(a);
            a->state = WlanAtvStateIdle;
            return;
        }
        furi_delay_ms(50);
    }

    a->state = WlanAtvStateBusy;
    int p2 = atv_pair_phase2(a, a->pin);
    atv_tls_close(a);
    a->state = (p2 == 0) ? WlanAtvStatePaired : WlanAtvStateError;
}

// ===========================================================================
// Remote control (RemoteMessage protocol, port 6466)
// ===========================================================================
static int atv_send_remote(WlanAndroidTv* a, int field, const uint8_t* sub, size_t sublen) {
    Pb m = {a->txbuf, 0, ATV_MSG_MAX};
    pb_bytes(&m, field, sub, sublen);
    if(m.len > ATV_MSG_MAX) return -1;
    return atv_send_msg(a, a->txbuf, m.len);
}

static int atv_send_key_inject(WlanAndroidTv* a, int keycode) {
    uint8_t sbuf[16];
    Pb ki = {sbuf, 0, sizeof(sbuf)};
    pb_uint(&ki, 1, (uint64_t)keycode); // key_code
    pb_uint(&ki, 2, 3); // direction = SHORT
    return atv_send_remote(a, 10, ki.p, ki.len); // remote_key_inject
}

// Handle one incoming RemoteMessage. Returns 1 if this was remote_start
// (session ready), 0 for handled/ignored, -1 on send error.
static int atv_handle_remote(WlanAndroidTv* a, uint32_t len, bool* got_start) {
    const uint8_t* buf = a->rxbuf;
    const uint8_t *sub, *sub2;
    uint32_t sublen, sub2len;

    if(pb_has(buf, len, 8)) { // remote_ping_request -> remote_ping_response
        uint64_t val1 = 0;
        if(pb_get_bytes(buf, len, 8, &sub, &sublen)) pb_get_uint(sub, sublen, 1, &val1);
        uint8_t pbuf[16];
        Pb pr = {pbuf, 0, sizeof(pbuf)};
        pb_uint(&pr, 1, val1);
        return atv_send_remote(a, 9, pr.p, pr.len) < 0 ? -1 : 0;
    }
    if(pb_has(buf, len, 1)) { // remote_configure -> reply + learn device name
        uint64_t supported = ATV_FEATURES;
        if(pb_get_bytes(buf, len, 1, &sub, &sublen)) {
            pb_get_uint(sub, sublen, 1, &supported); // code1
            if(pb_get_bytes(sub, sublen, 2, &sub2, &sub2len)) { // device_info
                const uint8_t *model, *vendor;
                uint32_t ml, vl;
                char m[24] = {0}, ve[24] = {0};
                if(pb_get_bytes(sub2, sub2len, 1, &model, &ml)) {
                    if(ml > sizeof(m) - 1) ml = sizeof(m) - 1;
                    memcpy(m, model, ml);
                }
                if(pb_get_bytes(sub2, sub2len, 2, &vendor, &vl)) {
                    if(vl > sizeof(ve) - 1) vl = sizeof(ve) - 1;
                    memcpy(ve, vendor, vl);
                }
                if(ve[0] || m[0]) {
                    snprintf(
                        a->device_name, sizeof(a->device_name), "%s%s%s", ve,
                        (ve[0] && m[0]) ? " " : "", m);
                }
            }
        }
        uint32_t code1 = (uint32_t)(supported & ATV_FEATURES);
        uint8_t dbuf[64];
        Pb di = {dbuf, 0, sizeof(dbuf)};
        pb_uint(&di, 3, 1); // unknown1
        pb_str(&di, 4, "1"); // unknown2
        pb_str(&di, 5, "atvremote"); // package_name
        pb_str(&di, 6, "1.0.0"); // app_version
        uint8_t rbuf[96];
        Pb rc = {rbuf, 0, sizeof(rbuf)};
        pb_uint(&rc, 1, code1);
        pb_bytes(&rc, 2, di.p, di.len);
        return atv_send_remote(a, 1, rc.p, rc.len) < 0 ? -1 : 0;
    }
    if(pb_has(buf, len, 2)) { // remote_set_active -> reply
        uint8_t sbuf[16];
        Pb sa = {sbuf, 0, sizeof(sbuf)};
        pb_uint(&sa, 1, ATV_FEATURES);
        return atv_send_remote(a, 2, sa.p, sa.len) < 0 ? -1 : 0;
    }
    if(pb_has(buf, len, 40)) { // remote_start -> session ready
        *got_start = true;
        return 1;
    }
    return 0; // volume/ime/... ignored
}

static void atv_do_connect(WlanAndroidTv* a) {
    a->state = WlanAtvStateBusy;
    a->error[0] = '\0';
    a->device_name[0] = '\0';

    // No stored cert -> we have never paired with any TV.
    if(!atv_cert_load(a)) {
        a->state = WlanAtvStateNeedsPair;
        return;
    }

    int rc = atv_tls_open(a, ATV_REMOTE_PORT);
    if(rc == -1) {
        atv_set_error(a, "TV unreachable");
        atv_tls_close(a);
        a->state = WlanAtvStateError;
        return;
    }
    if(rc == -2) {
        // TLS refused / reset -> the TV doesn't know our cert yet.
        atv_tls_close(a);
        a->state = WlanAtvStateNeedsPair;
        return;
    }

    // Remote handshake: reply to remote_configure / set_active until start.
    bool started = false;
    uint32_t start = furi_get_tick();
    while(!started) {
        uint32_t olen;
        int r = atv_recv_msg(a, &olen);
        if(r < 0) { // closed before start -> not paired
            atv_tls_close(a);
            a->state = WlanAtvStateNeedsPair;
            return;
        }
        if(r == 0) {
            if(a->cancel || atv_deadline_passed(start, 8000)) {
                atv_tls_close(a);
                a->state = a->cancel ? WlanAtvStateIdle : WlanAtvStateNeedsPair;
                return;
            }
            continue;
        }
        if(atv_handle_remote(a, olen, &started) < 0) {
            atv_tls_close(a);
            a->state = WlanAtvStateNeedsPair;
            return;
        }
    }

    // Session live: drain the key queue, answer pings, until cancel/drop.
    a->session_alive = true;
    a->state = WlanAtvStateConnected;
    while(!a->cancel) {
        int kc;
        while(xQueueReceive(a->key_queue, &kc, 0) == pdTRUE) {
            if(atv_send_key_inject(a, kc) < 0) {
                a->cancel = true;
                break;
            }
        }
        if(a->cancel) break;

        uint32_t olen;
        int r = atv_recv_msg(a, &olen);
        if(r < 0) { // connection dropped
            atv_set_error(a, "Connection lost");
            break;
        }
        if(r == 1) {
            bool ignore = false;
            if(atv_handle_remote(a, olen, &ignore) < 0) {
                atv_set_error(a, "Connection lost");
                break;
            }
        }
    }

    a->session_alive = false;
    atv_tls_close(a);
    // Cancel = user left -> Idle; otherwise a real drop -> Error.
    a->state = a->cancel ? WlanAtvStateIdle : WlanAtvStateError;
}

// ===========================================================================
// Worker task
// ===========================================================================
static void atv_worker(void* ctx) {
    WlanAndroidTv* a = ctx;
    AtvCmdType cmd;
    for(;;) {
        if(xQueueReceive(a->cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        if(cmd == AtvCmdQuit) break;
        a->cancel = false;
        // Flush any stale queued keys from a previous session.
        int junk;
        while(xQueueReceive(a->key_queue, &junk, 0) == pdTRUE) {
        }
        if(cmd == AtvCmdConnect)
            atv_do_connect(a);
        else if(cmd == AtvCmdPairStart)
            atv_do_pair(a);
    }
    vTaskDelete(NULL);
}

// ===========================================================================
// Public API
// ===========================================================================
WlanAndroidTv* wlan_androidtv_alloc(void) {
    WlanAndroidTv* a = heap_caps_calloc(1, sizeof(WlanAndroidTv), MALLOC_CAP_SPIRAM);
    if(!a) return NULL;
    a->state = WlanAtvStateIdle;
    a->storage = furi_record_open(RECORD_STORAGE);

    mbedtls_entropy_init(&a->entropy);
    mbedtls_ctr_drbg_init(&a->ctr_drbg);
    mbedtls_x509_crt_init(&a->client_crt);
    mbedtls_pk_init(&a->client_key);
    const char* seed = "wlan-androidtv";
    if(mbedtls_ctr_drbg_seed(
           &a->ctr_drbg, mbedtls_entropy_func, &a->entropy, (const unsigned char*)seed,
           strlen(seed)) != 0) {
        ESP_LOGE(TAG, "ctr_drbg seed failed");
        wlan_androidtv_free(a);
        return NULL;
    }

    a->rxbuf = heap_caps_malloc(ATV_MSG_MAX, MALLOC_CAP_SPIRAM);
    a->txbuf = heap_caps_malloc(ATV_MSG_MAX, MALLOC_CAP_SPIRAM);
    /* Worker stack in PSRAM. The TLS handshake needs a deep (20 KB) stack, but
     * the internal DRAM heap is too fragmented for a block that large once WiFi
     * has been global/sticky since boot (the permanent wlan_hal worker + wifi
     * service shape the heap early). This task only runs TLS + network I/O with
     * no flash operations while active, so an external-RAM stack is safe here
     * (CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY). The TCB stays in internal RAM
     * — it is touched from ISR context and must not sit in PSRAM. */
    a->stack = heap_caps_malloc(ATV_WORKER_STACK, MALLOC_CAP_SPIRAM);
    a->task_buf = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    a->cmd_queue = xQueueCreate(2, sizeof(AtvCmdType));
    a->key_queue = xQueueCreate(16, sizeof(int));

    if(!a->rxbuf || !a->txbuf || !a->stack || !a->task_buf || !a->cmd_queue || !a->key_queue) {
        ESP_LOGE(TAG, "alloc failed");
        wlan_androidtv_free(a);
        return NULL;
    }

    a->task = xTaskCreateStaticPinnedToCore(
        atv_worker, "AtvWorker", ATV_WORKER_STACK, a, 4, a->stack, a->task_buf, 0);
    if(!a->task) {
        ESP_LOGE(TAG, "task create failed");
        wlan_androidtv_free(a);
        return NULL;
    }
    return a;
}

void wlan_androidtv_free(WlanAndroidTv* a) {
    if(!a) return;
    if(a->task) {
        wlan_androidtv_disconnect(a);
        a->pin_ready = true; // release a pairing wait, if any
        AtvCmdType quit = AtvCmdQuit;
        xQueueSend(a->cmd_queue, &quit, portMAX_DELAY);
        for(int i = 0; i < 400 && eTaskGetState(a->task) != eDeleted; ++i) furi_delay_ms(5);
    }
    atv_tls_close(a);
    mbedtls_x509_crt_free(&a->client_crt);
    mbedtls_pk_free(&a->client_key);
    mbedtls_ctr_drbg_free(&a->ctr_drbg);
    mbedtls_entropy_free(&a->entropy);
    if(a->cmd_queue) vQueueDelete(a->cmd_queue);
    if(a->key_queue) vQueueDelete(a->key_queue);
    if(a->storage) furi_record_close(RECORD_STORAGE);
    if(a->rxbuf) free(a->rxbuf);
    if(a->txbuf) free(a->txbuf);
    if(a->stack) free(a->stack);
    if(a->task_buf) free(a->task_buf);
    free(a);
}

static void atv_enqueue(WlanAndroidTv* a, AtvCmdType cmd) {
    a->cancel = false;
    a->state = WlanAtvStateBusy;
    xQueueSend(a->cmd_queue, &cmd, portMAX_DELAY);
}

void wlan_androidtv_op_connect(WlanAndroidTv* a, const char* ip) {
    if(!a) return;
    strncpy(a->ip, ip, sizeof(a->ip) - 1);
    a->ip[sizeof(a->ip) - 1] = '\0';
    atv_enqueue(a, AtvCmdConnect);
}

void wlan_androidtv_op_pair_start(WlanAndroidTv* a, const char* ip) {
    if(!a) return;
    strncpy(a->ip, ip, sizeof(a->ip) - 1);
    a->ip[sizeof(a->ip) - 1] = '\0';
    a->pin_ready = false;
    atv_enqueue(a, AtvCmdPairStart);
}

void wlan_androidtv_op_pair_finish(WlanAndroidTv* a, const char* pin) {
    if(!a) return;
    strncpy(a->pin, pin, sizeof(a->pin) - 1);
    a->pin[sizeof(a->pin) - 1] = '\0';
    a->state = WlanAtvStateBusy;
    a->pin_ready = true; // release the pairing wait in atv_do_pair
}

void wlan_androidtv_send_key(WlanAndroidTv* a, int keycode) {
    if(!a) return;
    xQueueSend(a->key_queue, &keycode, 0);
}

WlanAtvState wlan_androidtv_state(WlanAndroidTv* a) {
    return a ? a->state : WlanAtvStateIdle;
}

const char* wlan_androidtv_error(WlanAndroidTv* a) {
    return a ? a->error : "";
}

void wlan_androidtv_device_name(WlanAndroidTv* a, char* out, size_t sz) {
    if(!a || sz == 0) return;
    strncpy(out, a->device_name, sz - 1);
    out[sz - 1] = '\0';
}

bool wlan_androidtv_is_connected(WlanAndroidTv* a) {
    return a && a->session_alive;
}

void wlan_androidtv_disconnect(WlanAndroidTv* a) {
    if(!a) return;
    a->cancel = true;
    a->pin_ready = true; // also release a pending pairing wait
    for(int i = 0; i < 400 && a->state == WlanAtvStateBusy; ++i) furi_delay_ms(10);
    for(int i = 0; i < 400 && a->session_alive; ++i) furi_delay_ms(10);
}
