#include "airplay_raop.h"
#include <esp_attr.h>

#include <lwip/sockets.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <furi.h>
#include <furi_hal_random.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define TAG "AirplayRaop"

#define RAOP_FRAMES_PER_PKT 352 /* RAOP standard frame length */
#define RAOP_SAMPLE_RATE 44100
#define RAOP_LATENCY 88200 /* 2 s @ 44.1 kHz — receiver playback delay */
#define RAOP_AUDIO_PORT 6000 /* our audio source port (fixed) */
#define RAOP_CTRL_PORT 6001 /* our control port (sync out / retransmit in) */
#define RAOP_TIMING_PORT 6002 /* our timing port (timing requests in) */
#define RB_FRAMES 22050 /* ~0.5 s stereo ring buffer (PSRAM) */
#define RTSP_BUF 1024
#define SENDER_STACK 6144 /* words; lwIP send/recv are deep on top of our buffers */

/* Big scratch buffers kept OFF the task stack (single sender task, no reentry).
 * Placed in the FAP's BSS (PSRAM) — lwIP copies into its own pbufs, so a PSRAM
 * source is fine, and it keeps the RAOP task stack small. */
static EXT_RAM_BSS_ATTR char s_rtsp_req[RTSP_BUF];
static int16_t s_pcmbuf[RAOP_FRAMES_PER_PKT * 2];
/* RTP header (12) + uncompressed-ALAC frame (23-bit hdr + 352*32 + 3 END, byte
 * aligned ≈ 1412) with headroom. */
static EXT_RAM_BSS_ATTR uint8_t s_pktbuf[12 + RAOP_FRAMES_PER_PKT * 4 + 32];
static volatile bool s_volume_dirty = false;

/* metadata / progress (sent to the receiver via SET_PARAMETER) */
static EXT_RAM_BSS_ATTR char s_meta_title[96];
static volatile bool s_meta_dirty = false;
static volatile uint32_t s_prog_elapsed_ms = 0;
static volatile uint32_t s_prog_duration_ms = 0;
static volatile bool s_prog_dirty = false;

/* worker stack/TCB must be internal DRAM in a FAP (see project memory) */
static StackType_t* s_task_stack = NULL;
static StaticTask_t* s_task_buf = NULL;
static TaskHandle_t s_task = NULL;

static volatile bool s_running = false;
static volatile bool s_task_done = false; /* task set this right before it deletes itself */
static volatile int s_handshake = 0; /* 0=pending, 1=ok, -1=fail */

static uint32_t s_recv_ip = 0;
static uint16_t s_rtsp_port = 0;
static uint32_t s_local_ip = 0;
static uint8_t s_volume = 80;

/* RTSP session state */
static int s_rtsp_sock = -1;
static int s_cseq = 0;
static EXT_RAM_BSS_ATTR char s_session[64];
static EXT_RAM_BSS_ATTR char s_client_instance[17];
static uint32_t s_session_id = 0;
static uint16_t s_server_port = 0; /* receiver audio port */
static uint16_t s_ctrl_port_remote = 0; /* receiver control port */
static uint16_t s_timing_port_remote = 0; /* receiver timing port */

/* RTP audio state */
static int s_audio_sock = -1;
static int s_ctrl_sock = -1;
static int s_timing_sock = -1;
static uint16_t s_seq = 0;
static uint32_t s_rtp_ts = 0;
static uint32_t s_ssrc = 0;
static volatile int s_timing_rx = 0; /* count of timing requests answered */
static volatile int s_ctrl_rx = 0; /* count of control-port packets (retransmit) */

/* PCM ring buffer (PSRAM), stereo int16 */
static int16_t* s_rb = NULL;
static volatile size_t s_rb_head = 0, s_rb_tail = 0, s_rb_count = 0; /* in frames */
static FuriMutex* s_rb_mtx = NULL;

/* ---------------- helpers ---------------- */

static inline uint16_t bswap16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}
static inline uint32_t bswap32(uint32_t v) {
    return ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | ((v >> 24) & 0xff);
}

static void base64_encode(const uint8_t* in, int len, char* out) {
    static const char* t =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i = 0, o = 0;
    for(; i + 2 < len; i += 3) {
        out[o++] = t[in[i] >> 2];
        out[o++] = t[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        out[o++] = t[((in[i + 1] & 15) << 2) | (in[i + 2] >> 6)];
        out[o++] = t[in[i + 2] & 63];
    }
    if(len - i == 1) {
        out[o++] = t[in[i] >> 2];
        out[o++] = t[(in[i] & 3) << 4];
        out[o++] = '=';
        out[o++] = '=';
    } else if(len - i == 2) {
        out[o++] = t[in[i] >> 2];
        out[o++] = t[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        out[o++] = t[(in[i + 1] & 15) << 2];
        out[o++] = '=';
    }
    out[o] = '\0';
}

/* Monotonic NTP-format timestamp (sec.frac fixed-point). Add a 1900-epoch base
 * (~2026) so the value looks like a real NTP time — some receivers sanity-check
 * the sync packet's timestamp and drop it if it's implausibly small. */
#define NTP_EPOCH_1900 3976300800ULL
static uint64_t ntp_time(void) {
    int64_t us = esp_timer_get_time();
    uint64_t sec = (uint64_t)(us / 1000000LL) + NTP_EPOCH_1900;
    uint64_t frac = (uint64_t)((us % 1000000LL) * 4294967296ULL / 1000000ULL);
    return (sec << 32) | frac;
}

static void ip_to_str(uint32_t ip_be, char* out, size_t sz) {
    snprintf(
        out, sz, "%u.%u.%u.%u", (unsigned)(ip_be & 0xff), (unsigned)((ip_be >> 8) & 0xff),
        (unsigned)((ip_be >> 16) & 0xff), (unsigned)((ip_be >> 24) & 0xff));
}

static void raop_send_volume(void); /* all defined below; sender-task only */
static void raop_send_metadata(void);
static void raop_send_progress(void);
static void rtsp_send_binary(
    const char* uri,
    const char* extra_headers,
    const uint8_t* body,
    int body_len);

/* ---------------- RTSP ---------------- */

/* Send one RTSP request and read the response into resp. Returns the numeric
 * status (e.g. 200) or -1 on socket error. */
static int rtsp_transact(
    const char* method,
    const char* uri,
    const char* extra_headers,
    const char* body,
    char* resp,
    size_t resp_sz) {
    char* req = s_rtsp_req;
    int blen = body ? (int)strlen(body) : 0;
    int n;
    if(blen) {
        n = snprintf(
            req, RTSP_BUF,
            "%s %s RTSP/1.0\r\n"
            "CSeq: %d\r\n"
            "User-Agent: iTunes/12.8.0 (Macintosh; OS X 10.14.6)\r\n"
            "Client-Instance: %s\r\n"
            "DACP-ID: %s\r\n"
            "Active-Remote: 1\r\n"
            "%s"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            method, uri, ++s_cseq, s_client_instance, s_client_instance,
            extra_headers ? extra_headers : "", blen, body);
    } else {
        n = snprintf(
            req, RTSP_BUF,
            "%s %s RTSP/1.0\r\n"
            "CSeq: %d\r\n"
            "User-Agent: iTunes/12.8.0 (Macintosh; OS X 10.14.6)\r\n"
            "Client-Instance: %s\r\n"
            "DACP-ID: %s\r\n"
            "Active-Remote: 1\r\n"
            "%s"
            "\r\n",
            method, uri, ++s_cseq, s_client_instance, s_client_instance,
            extra_headers ? extra_headers : "");
    }

    if(lwip_send(s_rtsp_sock, req, n, 0) != n) {
        ESP_LOGE(TAG, "%s send failed", method);
        return -1;
    }

    int total = 0;
    resp[0] = '\0';
    /* read until we have the header terminator (good enough for these short
     * responses that carry no large body). */
    while(total < (int)resp_sz - 1) {
        int r = lwip_recv(s_rtsp_sock, resp + total, resp_sz - 1 - total, 0);
        if(r <= 0) break;
        total += r;
        resp[total] = '\0';
        if(strstr(resp, "\r\n\r\n")) break;
    }
    if(total <= 0) {
        ESP_LOGE(TAG, "%s no response", method);
        return -1;
    }

    int code = -1;
    /* status line: "RTSP/1.0 200 OK" */
    const char* sp = strchr(resp, ' ');
    if(sp) code = atoi(sp + 1);
    ESP_LOGI(TAG, "%s -> %d", method, code);
    return code;
}

static bool rtsp_handshake(void) {
    char uri[64];
    char resp[RTSP_BUF];
    char hdr[256];
    char local_str[16];
    ip_to_str(s_local_ip, local_str, sizeof(local_str));
    snprintf(uri, sizeof(uri), "rtsp://%s/%lu", local_str, (unsigned long)s_session_id);

    /* OPTIONS with Apple-Challenge — many receivers only "activate" a session
     * once they've seen a challenge from what looks like a real AirPlay client.
     * We don't verify the Apple-Response, we just need to send the challenge. */
    uint8_t chal[16];
    for(int i = 0; i < 16; i++) chal[i] = (uint8_t)(furi_hal_random_get() & 0xff);
    char chal_b64[28];
    base64_encode(chal, 16, chal_b64);
    char opt_hdr[64];
    snprintf(opt_hdr, sizeof(opt_hdr), "Apple-Challenge: %s\r\n", chal_b64);
    if(rtsp_transact("OPTIONS", "*", opt_hdr, NULL, resp, sizeof(resp)) != 200) return false;

    /* ANNOUNCE with SDP (PCM L16, unencrypted) */
    char recv_str[16];
    ip_to_str(s_recv_ip, recv_str, sizeof(recv_str));
    char sdp[512];
    snprintf(
        sdp, sizeof(sdp),
        "v=0\r\n"
        "o=iTunes %lu 0 IN IP4 %s\r\n"
        "s=iTunes\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "m=audio 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 AppleLossless\r\n"
        "a=fmtp:96 %d 0 16 40 10 14 2 255 0 0 44100\r\n",
        (unsigned long)s_session_id, local_str, recv_str, RAOP_FRAMES_PER_PKT);
    if(rtsp_transact(
           "ANNOUNCE", uri, "Content-Type: application/sdp\r\n", sdp, resp, sizeof(resp)) != 200)
        return false;

    /* SETUP — advertise our control/timing ports, learn the receiver's ports */
    snprintf(
        hdr, sizeof(hdr),
        "Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;"
        "control_port=%d;timing_port=%d\r\n",
        RAOP_CTRL_PORT, RAOP_TIMING_PORT);
    if(rtsp_transact("SETUP", uri, hdr, NULL, resp, sizeof(resp)) != 200) return false;

    /* parse server_port / control_port and Session from the response */
    const char* p = strstr(resp, "server_port=");
    if(p) s_server_port = (uint16_t)atoi(p + 12);
    p = strstr(resp, "control_port=");
    if(p) s_ctrl_port_remote = (uint16_t)atoi(p + 13);
    p = strstr(resp, "timing_port=");
    if(p) s_timing_port_remote = (uint16_t)atoi(p + 12);
    p = strstr(resp, "Session:");
    if(p) {
        p += 8;
        while(*p == ' ') p++;
        int i = 0;
        while(*p && *p != '\r' && *p != '\n' && *p != ';' && i < (int)sizeof(s_session) - 1) {
            s_session[i++] = *p++;
        }
        s_session[i] = '\0';
    }
    ESP_LOGI(
        TAG, "server_port=%u ctrl=%u session=%s", s_server_port, s_ctrl_port_remote, s_session);
    if(!s_server_port) return false;

    /* RECORD */
    snprintf(
        hdr, sizeof(hdr),
        "Session: %s\r\n"
        "Range: npt=0-\r\n"
        "RTP-Info: seq=%u;rtptime=%lu\r\n",
        s_session, s_seq, (unsigned long)s_rtp_ts);
    if(rtsp_transact("RECORD", uri, hdr, NULL, resp, sizeof(resp)) != 200) return false;

    /* SET_PARAMETER volume + initial metadata */
    raop_send_volume();
    raop_send_metadata();
    return true;
}

static void rtsp_teardown(void) {
    if(s_rtsp_sock < 0) return;
    char uri[64], resp[RTSP_BUF], hdr[80], local_str[16];
    ip_to_str(s_local_ip, local_str, sizeof(local_str));
    snprintf(uri, sizeof(uri), "rtsp://%s/%lu", local_str, (unsigned long)s_session_id);
    snprintf(hdr, sizeof(hdr), "Session: %s\r\n", s_session);
    rtsp_transact("TEARDOWN", uri, hdr, NULL, resp, sizeof(resp));
}

/* ---------------- UDP audio / sync / timing ---------------- */

static int udp_bind(uint16_t port) {
    int s = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(s < 0) return -1;
    int yes = 1;
    lwip_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = bswap16(port);
    a.sin_addr.s_addr = 0; /* INADDR_ANY — receive regardless of local address */
    if(lwip_bind(s, (struct sockaddr*)&a, sizeof(a)) < 0) {
        lwip_close(s);
        return -1;
    }
    /* non-blocking so the sender loop can poll */
    int nb = 1;
    lwip_ioctl(s, FIONBIO, &nb);
    return s;
}

static void send_sync(bool first) {
    if(s_ctrl_sock < 0) return;
    uint8_t pkt[20];
    pkt[0] = first ? 0x90 : 0x80;
    pkt[1] = 0xd4; /* PT 84 + marker */
    pkt[2] = 0x00;
    pkt[3] = 0x07;
    uint32_t now_ts = s_rtp_ts;
    uint32_t ts_lat = now_ts - RAOP_LATENCY;
    uint64_t ntp = ntp_time();
    uint32_t w;
    w = bswap32(ts_lat);
    memcpy(pkt + 4, &w, 4);
    uint32_t hi = bswap32((uint32_t)(ntp >> 32)), lo = bswap32((uint32_t)(ntp & 0xffffffff));
    memcpy(pkt + 8, &hi, 4);
    memcpy(pkt + 12, &lo, 4);
    w = bswap32(now_ts);
    memcpy(pkt + 16, &w, 4);

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = bswap16(s_ctrl_port_remote);
    dst.sin_addr.s_addr = s_recv_ip;
    lwip_sendto(s_ctrl_sock, pkt, sizeof(pkt), 0, (struct sockaddr*)&dst, sizeof(dst));
}

/* Sender-initiated NTP timing request (0xd2) to the receiver's timing port.
 * Some receivers never poll us; kicking the handshake ourselves establishes the
 * clock sync they need before they'll actually render the audio. */
static void send_timing_request(void) {
    if(s_timing_sock < 0 || !s_timing_port_remote) return;
    uint8_t pkt[32];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x80;
    pkt[1] = 0xd2; /* timing request */
    pkt[2] = 0x00;
    pkt[3] = 0x07;
    uint64_t ntp = ntp_time();
    uint32_t hi = bswap32((uint32_t)(ntp >> 32)), lo = bswap32((uint32_t)ntp);
    memcpy(pkt + 24, &hi, 4); /* our transmit timestamp */
    memcpy(pkt + 28, &lo, 4);
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = bswap16(s_timing_port_remote);
    dst.sin_addr.s_addr = s_recv_ip;
    lwip_sendto(s_timing_sock, pkt, sizeof(pkt), 0, (struct sockaddr*)&dst, sizeof(dst));
}

static void poll_timing(void) {
    if(s_timing_sock < 0) return;
    uint8_t req[64];
    struct sockaddr_in src;
    socklen_t sl = sizeof(src);
    int n = lwip_recvfrom(s_timing_sock, req, sizeof(req), 0, (struct sockaddr*)&src, &sl);
    if(n < 32) return;
    s_timing_rx++;
    if(req[1] == 0xd3) return; /* reply to our own request — just count it */
    uint64_t recv_ntp = ntp_time();
    uint8_t rep[32];
    memset(rep, 0, sizeof(rep));
    rep[0] = 0x80;
    rep[1] = 0xd3; /* timing reply, PT 83 + marker */
    rep[2] = 0x00;
    rep[3] = 0x07;
    memcpy(rep + 8, req + 24, 8); /* origin = request transmit ts */
    uint32_t hi = bswap32((uint32_t)(recv_ntp >> 32)), lo = bswap32((uint32_t)(recv_ntp));
    memcpy(rep + 16, &hi, 4);
    memcpy(rep + 20, &lo, 4);
    uint64_t tx = ntp_time();
    hi = bswap32((uint32_t)(tx >> 32));
    lo = bswap32((uint32_t)(tx));
    memcpy(rep + 24, &hi, 4);
    memcpy(rep + 28, &lo, 4);
    lwip_sendto(s_timing_sock, rep, sizeof(rep), 0, (struct sockaddr*)&src, sl);
}

/* Drain the control socket (receiver retransmit / feedback). We don't act on
 * retransmit requests yet — this just tells us whether the receiver talks back
 * at all, and keeps the socket buffer clear. */
static void poll_control(void) {
    if(s_ctrl_sock < 0) return;
    uint8_t buf[256];
    struct sockaddr_in src;
    socklen_t sl = sizeof(src);
    int n = lwip_recvfrom(s_ctrl_sock, buf, sizeof(buf), 0, (struct sockaddr*)&src, &sl);
    if(n > 0) s_ctrl_rx++;
}

/* pop up to RAOP_FRAMES_PER_PKT frames into out (stereo int16); returns frames */
static size_t rb_pop(int16_t* out) {
    furi_mutex_acquire(s_rb_mtx, FuriWaitForever);
    size_t n = s_rb_count < RAOP_FRAMES_PER_PKT ? s_rb_count : RAOP_FRAMES_PER_PKT;
    for(size_t i = 0; i < n; i++) {
        out[i * 2 + 0] = s_rb[s_rb_tail * 2 + 0];
        out[i * 2 + 1] = s_rb[s_rb_tail * 2 + 1];
        s_rb_tail = (s_rb_tail + 1) % RB_FRAMES;
    }
    s_rb_count -= n;
    furi_mutex_release(s_rb_mtx);
    return n;
}

/* MSB-first bit writer over a pre-zeroed buffer. */
typedef struct {
    uint8_t* buf;
    int bitpos;
} BitW;
static void bw_put(BitW* b, uint32_t val, int nbits) {
    for(int i = nbits - 1; i >= 0; i--) {
        if((val >> i) & 1u) b->buf[b->bitpos >> 3] |= (uint8_t)(1u << (7 - (b->bitpos & 7)));
        b->bitpos++;
    }
}

static void send_audio_packet(const int16_t* stereo, size_t frames, bool first) {
    if(s_audio_sock < 0) return;
    uint8_t* pkt = s_pktbuf;
    pkt[0] = 0x80;
    pkt[1] = first ? 0xe0 : 0x60; /* PT 96, marker on first */
    uint16_t seq_be = bswap16(s_seq);
    memcpy(pkt + 2, &seq_be, 2);
    uint32_t ts_be = bswap32(s_rtp_ts);
    memcpy(pkt + 4, &ts_be, 4);
    uint32_t ssrc_be = bswap32(s_ssrc);
    memcpy(pkt + 8, &ssrc_be, 4);

    /* Uncompressed-ALAC payload: one CPE (stereo) escape frame, always exactly
     * RAOP_FRAMES_PER_PKT samples (zero-pad a short tail). The header bits start
     * 0x20 — the well-known raw-ALAC frame marker. */
    uint8_t* pl = pkt + 12;
    int paymax = (int)sizeof(s_pktbuf) - 12;
    memset(pl, 0, paymax);
    BitW bw = {pl, 0};
    bw_put(&bw, 1, 3); /* element = CPE (channel pair) */
    bw_put(&bw, 0, 4); /* element instance tag */
    bw_put(&bw, 0, 12); /* unused */
    bw_put(&bw, 0, 1); /* partialFrame = 0 (full frame) */
    bw_put(&bw, 0, 2); /* bytesShifted = 0 */
    bw_put(&bw, 1, 1); /* escapeFlag = 1 (uncompressed) */
    for(int i = 0; i < RAOP_FRAMES_PER_PKT; i++) {
        int16_t l = (i < (int)frames) ? stereo[i * 2 + 0] : 0;
        int16_t r = (i < (int)frames) ? stereo[i * 2 + 1] : 0;
        bw_put(&bw, (uint16_t)l, 16);
        bw_put(&bw, (uint16_t)r, 16);
    }
    bw_put(&bw, 7, 3); /* ID_END */
    int paylen = (bw.bitpos + 7) / 8; /* byte-align */

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = bswap16(s_server_port);
    dst.sin_addr.s_addr = s_recv_ip;
    lwip_sendto(s_audio_sock, pkt, 12 + paylen, 0, (struct sockaddr*)&dst, sizeof(dst));
}

/* ---------------- sender task ---------------- */

static void raop_task(void* arg) {
    UNUSED(arg);

    /* 1) RTSP handshake */
    s_rtsp_sock = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(s_rtsp_sock < 0) {
        s_handshake = -1;
        s_running = false;
        s_task_done = true;
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in ra = {0};
    ra.sin_family = AF_INET;
    ra.sin_port = bswap16(s_rtsp_port);
    ra.sin_addr.s_addr = s_recv_ip;
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    lwip_setsockopt(s_rtsp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if(lwip_connect(s_rtsp_sock, (struct sockaddr*)&ra, sizeof(ra)) < 0) {
        ESP_LOGE(TAG, "RTSP connect failed");
        lwip_close(s_rtsp_sock);
        s_rtsp_sock = -1;
        s_handshake = -1;
        s_running = false;
        s_task_done = true;
        vTaskDelete(NULL);
        return;
    }

    if(!rtsp_handshake()) {
        ESP_LOGE(TAG, "handshake failed");
        lwip_close(s_rtsp_sock);
        s_rtsp_sock = -1;
        s_handshake = -1;
        s_running = false;
        s_task_done = true;
        vTaskDelete(NULL);
        return;
    }

    /* 2) open UDP sockets (audio bound to a fixed source port too) */
    s_audio_sock = udp_bind(RAOP_AUDIO_PORT);
    s_ctrl_sock = udp_bind(RAOP_CTRL_PORT);
    s_timing_sock = udp_bind(RAOP_TIMING_PORT);
    ESP_LOGI(TAG, "streaming: audio->%u ctrl->%u", s_server_port, s_ctrl_port_remote);

    s_handshake = 1; /* success — release the caller */

    /* 3) stream loop, paced at real time */
    int16_t* pcmbuf = s_pcmbuf;
    int64_t start_us = esp_timer_get_time();
    uint64_t samples_sent = 0;
    int64_t last_sync_us = 0;
    bool first_pkt = true;
    bool first_sync = true;

    while(s_running) {
        poll_timing();
        poll_control();

        int64_t now = esp_timer_get_time();
        if(now - last_sync_us >= 1000000) { /* sync once per second */
            send_sync(first_sync);
            send_timing_request();
            first_sync = false;
            last_sync_us = now;
        }

        if(s_volume_dirty) {
            s_volume_dirty = false;
            raop_send_volume();
        }
        if(s_meta_dirty) {
            s_meta_dirty = false;
            raop_send_metadata();
        }
        if(s_prog_dirty) {
            s_prog_dirty = false;
            raop_send_progress();
        }

        /* how many samples should have been sent by now (+ small prebuffer) */
        uint64_t target =
            (uint64_t)((now - start_us) * (int64_t)RAOP_SAMPLE_RATE / 1000000LL) + RAOP_FRAMES_PER_PKT;

        int sent_this_round = 0;
        while(samples_sent < target && sent_this_round < 8) {
            size_t n = rb_pop(pcmbuf);
            if(n == 0) break; /* ring empty — wait for the decoder */
            send_audio_packet(pcmbuf, n, first_pkt);
            first_pkt = false;
            s_seq++;
            /* one packet = one full ALAC frame (short tail is zero-padded) */
            s_rtp_ts += RAOP_FRAMES_PER_PKT;
            samples_sent += RAOP_FRAMES_PER_PKT;
            sent_this_round++;
        }

        furi_delay_ms(2);
    }

    /* 4) teardown — short recv timeout so TEARDOWN can't block the stop path */
    struct timeval tvq = {.tv_sec = 0, .tv_usec = 300 * 1000};
    if(s_rtsp_sock >= 0) lwip_setsockopt(s_rtsp_sock, SOL_SOCKET, SO_RCVTIMEO, &tvq, sizeof(tvq));
    rtsp_teardown();
    if(s_audio_sock >= 0) lwip_close(s_audio_sock);
    if(s_ctrl_sock >= 0) lwip_close(s_ctrl_sock);
    if(s_timing_sock >= 0) lwip_close(s_timing_sock);
    if(s_rtsp_sock >= 0) lwip_close(s_rtsp_sock);
    s_audio_sock = s_ctrl_sock = s_timing_sock = s_rtsp_sock = -1;
    ESP_LOGI(TAG, "sender stopped");
    s_task_done = true;
    vTaskDelete(NULL);
}

/* ---------------- public API ---------------- */

bool airplay_raop_start(uint32_t receiver_ip, uint16_t rtsp_port, uint32_t local_ip) {
    if(s_running) return true;

    s_recv_ip = receiver_ip;
    s_rtsp_port = rtsp_port;
    s_local_ip = local_ip;
    s_cseq = 0;
    s_session[0] = '\0';
    s_server_port = 0;
    s_ctrl_port_remote = 0;
    s_handshake = 0;

    /* random session identifiers */
    s_session_id = furi_hal_random_get();
    s_ssrc = furi_hal_random_get();
    s_seq = (uint16_t)furi_hal_random_get();
    s_rtp_ts = furi_hal_random_get();
    for(int i = 0; i < 16; i++) {
        s_client_instance[i] = "0123456789ABCDEF"[furi_hal_random_get() & 0xf];
    }
    s_client_instance[16] = '\0';

    /* ring buffer (PSRAM) + mutex */
    s_rb = heap_caps_malloc(RB_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if(!s_rb) s_rb = malloc(RB_FRAMES * 2 * sizeof(int16_t));
    if(!s_rb) return false;
    s_rb_head = s_rb_tail = s_rb_count = 0;
    if(!s_rb_mtx) s_rb_mtx = furi_mutex_alloc(FuriMutexTypeNormal);

    /* sender task with internal-DRAM stack + TCB (FAP requirement) */
    s_task_stack =
        heap_caps_malloc(SENDER_STACK * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_task_buf = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!s_task_stack || !s_task_buf) {
        free(s_task_stack);
        s_task_stack = NULL;
        free(s_task_buf);
        s_task_buf = NULL;
        free(s_rb);
        s_rb = NULL;
        return false;
    }

    s_task_done = false;
    s_timing_rx = 0;
    s_ctrl_rx = 0;
    s_running = true;
    s_task = xTaskCreateStaticPinnedToCore(
        raop_task, "AirplayRaop", SENDER_STACK, NULL, 6, s_task_stack, s_task_buf, 0);

    /* wait for the handshake result (task sets s_handshake) */
    uint32_t waited = 0;
    while(s_handshake == 0 && waited < 8000) {
        furi_delay_ms(20);
        waited += 20;
    }
    if(s_handshake != 1) {
        airplay_raop_stop();
        return false;
    }
    return true;
}

void airplay_raop_stop(void) {
    if(s_running) {
        s_running = false;
        /* Wait until the task has finished its teardown and set s_task_done
         * BEFORE freeing its stack/TCB/ring — otherwise the still-running task
         * writes to freed memory (StoreProhibited crash). */
        uint32_t waited = 0;
        while(!s_task_done && waited < 8000) {
            furi_delay_ms(10);
            waited += 10;
        }
        furi_delay_ms(30); /* let the scheduler reclaim the deleted task */
        s_task = NULL;
    }
    if(s_task_stack) {
        free(s_task_stack);
        s_task_stack = NULL;
    }
    if(s_task_buf) {
        free(s_task_buf);
        s_task_buf = NULL;
    }
    if(s_rb) {
        free(s_rb);
        s_rb = NULL;
    }
    s_handshake = 0;
}

bool airplay_raop_is_active(void) {
    return s_running && s_handshake == 1;
}

size_t airplay_raop_push(const int16_t* stereo_pcm, size_t n_frames, uint32_t timeout_ms) {
    if(!s_rb || !s_running) return 0;
    size_t written = 0;
    uint32_t waited = 0;
    while(written < n_frames) {
        furi_mutex_acquire(s_rb_mtx, FuriWaitForever);
        while(written < n_frames && s_rb_count < RB_FRAMES) {
            s_rb[s_rb_head * 2 + 0] = stereo_pcm[written * 2 + 0];
            s_rb[s_rb_head * 2 + 1] = stereo_pcm[written * 2 + 1];
            s_rb_head = (s_rb_head + 1) % RB_FRAMES;
            s_rb_count++;
            written++;
        }
        furi_mutex_release(s_rb_mtx);
        if(written >= n_frames) break;
        if(waited >= timeout_ms) break;
        furi_delay_ms(5);
        waited += 5;
    }
    return written;
}

void airplay_raop_flush(void) {
    if(!s_rb_mtx) return;
    furi_mutex_acquire(s_rb_mtx, FuriWaitForever);
    s_rb_head = s_rb_tail = s_rb_count = 0;
    furi_mutex_release(s_rb_mtx);
}

bool airplay_raop_has_pending(void) {
    return s_rb_count > 0;
}

/* Sends the current volume to the receiver via SET_PARAMETER. Uses lwIP, so it
 * must ONLY be called from the sender task (handshake + dirty-flag poll). */
static void raop_send_volume(void) {
    if(s_rtsp_sock < 0) return;
    /* map 0..100 → AirPlay dB: 0 => -144 (mute), else -30..0 */
    float db;
    if(s_volume == 0) {
        db = -144.0f;
    } else {
        db = -30.0f + (30.0f * (float)s_volume / 100.0f);
    }
    char body[40];
    snprintf(body, sizeof(body), "volume: %.6f\r\n", (double)db);
    char uri[64], resp[RTSP_BUF], hdr[96], local_str[16];
    ip_to_str(s_local_ip, local_str, sizeof(local_str));
    snprintf(uri, sizeof(uri), "rtsp://%s/%lu", local_str, (unsigned long)s_session_id);
    snprintf(hdr, sizeof(hdr), "Session: %s\r\nContent-Type: text/parameters\r\n", s_session);
    rtsp_transact("SET_PARAMETER", uri, hdr, body, resp, sizeof(resp));
}

/* Callable from any thread — just records the value; the sender task pushes it
 * to the receiver (keeps lwIP off the UI thread). */
void airplay_raop_set_volume(uint8_t volume) {
    s_volume = volume;
    s_volume_dirty = true;
}

/* SET_PARAMETER with a BINARY body (DAAP contains NUL bytes, so it can't go
 * through rtsp_transact()'s strlen-based path). Sender-task only. */
static void rtsp_send_binary(
    const char* uri,
    const char* extra_headers,
    const uint8_t* body,
    int body_len) {
    if(s_rtsp_sock < 0) return;
    char* req = s_rtsp_req;
    int n = snprintf(
        req, RTSP_BUF,
        "SET_PARAMETER %s RTSP/1.0\r\n"
        "CSeq: %d\r\n"
        "User-Agent: iTunes/12.8.0 (Macintosh; OS X 10.14.6)\r\n"
        "Client-Instance: %s\r\n"
        "DACP-ID: %s\r\n"
        "Active-Remote: 1\r\n"
        "%s"
        "Content-Length: %d\r\n"
        "\r\n",
        uri, ++s_cseq, s_client_instance, s_client_instance,
        extra_headers ? extra_headers : "", body_len);
    if(n <= 0 || n + body_len > RTSP_BUF) return;
    memcpy(req + n, body, body_len);
    lwip_send(s_rtsp_sock, req, n + body_len, 0);
    char resp[128];
    lwip_recv(s_rtsp_sock, resp, sizeof(resp) - 1, 0); /* drain the response */
}

/* DAAP-tagged track title → receiver display. Sender-task only. */
static void raop_send_metadata(void) {
    if(s_rtsp_sock < 0 || !s_meta_title[0]) return;
    int tl = (int)strlen(s_meta_title);
    if(tl > 80) tl = 80;
    /* mlit { minm <title> } */
    uint8_t daap[128];
    int p = 0;
    memcpy(daap + p, "mlit", 4);
    p += 4;
    uint32_t ilen = 8 + (uint32_t)tl;
    daap[p++] = (uint8_t)(ilen >> 24);
    daap[p++] = (uint8_t)(ilen >> 16);
    daap[p++] = (uint8_t)(ilen >> 8);
    daap[p++] = (uint8_t)ilen;
    memcpy(daap + p, "minm", 4);
    p += 4;
    daap[p++] = (uint8_t)(tl >> 24);
    daap[p++] = (uint8_t)(tl >> 16);
    daap[p++] = (uint8_t)(tl >> 8);
    daap[p++] = (uint8_t)tl;
    memcpy(daap + p, s_meta_title, tl);
    p += tl;

    char uri[64], local_str[16], hdr[160];
    ip_to_str(s_local_ip, local_str, sizeof(local_str));
    snprintf(uri, sizeof(uri), "rtsp://%s/%lu", local_str, (unsigned long)s_session_id);
    snprintf(
        hdr, sizeof(hdr),
        "Session: %s\r\nContent-Type: application/x-dmap-tagged\r\nRTP-Info: rtptime=%lu\r\n",
        s_session, (unsigned long)s_rtp_ts);
    rtsp_send_binary(uri, hdr, daap, p);
}

/* progress: start/current/end (RTP timestamps) → receiver scrubber. */
static void raop_send_progress(void) {
    if(s_rtsp_sock < 0 || s_prog_duration_ms == 0) return;
    uint32_t cur = s_rtp_ts;
    uint32_t elapsed_s = (uint32_t)((uint64_t)s_prog_elapsed_ms * RAOP_SAMPLE_RATE / 1000);
    uint32_t dur_s = (uint32_t)((uint64_t)s_prog_duration_ms * RAOP_SAMPLE_RATE / 1000);
    uint32_t start = cur - elapsed_s;
    uint32_t end = start + dur_s;
    char body[64];
    snprintf(
        body, sizeof(body), "progress: %lu/%lu/%lu\r\n", (unsigned long)start, (unsigned long)cur,
        (unsigned long)end);
    char uri[64], local_str[16], hdr[96], resp[RTSP_BUF];
    ip_to_str(s_local_ip, local_str, sizeof(local_str));
    snprintf(uri, sizeof(uri), "rtsp://%s/%lu", local_str, (unsigned long)s_session_id);
    snprintf(hdr, sizeof(hdr), "Session: %s\r\nContent-Type: text/parameters\r\n", s_session);
    rtsp_transact("SET_PARAMETER", uri, hdr, body, resp, sizeof(resp));
}

void airplay_raop_set_metadata(const char* title, uint32_t duration_ms) {
    strncpy(s_meta_title, title ? title : "", sizeof(s_meta_title) - 1);
    s_meta_title[sizeof(s_meta_title) - 1] = '\0';
    s_prog_duration_ms = duration_ms;
    s_meta_dirty = true;
}

void airplay_raop_set_progress(uint32_t elapsed_ms, uint32_t duration_ms) {
    s_prog_elapsed_ms = elapsed_ms;
    s_prog_duration_ms = duration_ms;
    s_prog_dirty = true;
}
