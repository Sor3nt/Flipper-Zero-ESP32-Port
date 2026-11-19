/**
 * Background qFlipper bridge — see qflipper_bridge.h.
 *
 * This is the former standalone "qFlipper" app (applications/main/qflipper)
 * turned into a background service owned by the desktop: the composite + RPC
 * pipe run in a dedicated FuriThread instead of a foreground view, so the lock
 * menu can toggle it on/off while the user keeps using the desktop.
 *
 * Installing the TinyUSB composite switches the ESP32-S3 internal USB PHY from
 * USB-Serial-JTAG to USB-OTG, which kills the serial/JTAG bridge esptool uses
 * for flashing. So we only do it on demand.
 *
 * stop() here only detaches the RPC bridge (CDC callbacks off, session closed)
 * and deliberately LEAVES the composite installed — it is also the mutual-
 * exclusion hook the USB-Storage scene calls before taking the composite over
 * for MSC, and tearing it down there would just churn (uninstall + reinstall).
 * The *user-facing* "Disable qFlipper" path (lock menu) calls stop() AND then
 * furi_hal_usb_composite_uninstall(), which fully tears the composite down and
 * routes the internal PHY back to USB-Serial-JTAG — so JTAG returns live, no
 * reboot. That uninstall is now a clean, symmetric teardown (frees the
 * esp_tinyusb CDC-ACM wrapper), so re-enabling qFlipper reinstalls cleanly.
 *
 * The PHY mux also lives in the RTC always-on domain and survives a soft reset,
 * so as a safety net furi_hal_usb_init() forces the mux back to USJ at every
 * boot — even a crash/watchdog reboot with the composite still up recovers the
 * flash/console port without a BOOT+RESET.
 */

#include "qflipper_bridge.h"
#include <momentum_settings.h>

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_cdc.h>
#include <rpc/rpc.h>

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <furi_hal_sd.h>
#include <esp_system.h> /* esp_reset_reason() for the QFDIAG dump header */
#include <furi_hal_qfdiag.h>
#include <storage/storage.h>

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2
#include "furi_hal_usb_tinyusb_composite.h"
#include "furi_hal_usb_msc.h"
#define QFLIPPER_HAVE_COMPOSITE 1
#else
#define QFLIPPER_HAVE_COMPOSITE 0
#endif

#define TAG "qFlipperBridge"

#define USB_RPC_CDC_ITF       0
/* Larger drain chunks shorten the race window between the avail-check and
 * the FIFO read in usb_rpc_drain_rx — fewer iterations, fewer chances for
 * the host to slip a byte into the FIFO that we'd then drop. */
#define USB_RPC_RX_CHUNK_SIZE 2048

/* While connected and piping RPC, the loop also re-drains the CDC FIFO on this
 * period. RxAvailable events are posted non-blocking and are dropped when the
 * 16-deep event queue is full (e.g. during a sustained file upload), so without
 * this poll a dropped event could leave inbound bytes stranded in the FIFO and
 * stall the transfer — qFlipper then reports "connection lost". The drain is a
 * cheap no-op when the FIFO is empty. */
#define USB_RPC_RX_POLL_MS    5

/* Internal events */
typedef enum {
    UsbRpcEventConnected,    /* DTR went high */
    UsbRpcEventDisconnected, /* DTR went low or USB disconnect */
    UsbRpcEventRxAvailable,  /* CDC RX bytes ready */
    UsbRpcEventTxComplete,   /* CDC TX done */
    UsbRpcEventExit,         /* stop() requested — leave the loop */
} UsbRpcEvent;

typedef struct {
    FuriThread* thread;
    FuriMessageQueue* event_q;

    Rpc* rpc;
    RpcSession* session;

    bool connected;       /* DTR state */
    bool session_open;    /* RPC session active */
    bool rpc_mode;        /* false = CLI handshake, true = piping protobuf */
    bool auto_off_armed;  /* host was connected and dropped DTR — grace period running */
    bool exiting; /* stop() requested — bail out of any blocking loop */

    /* CLI handshake line buffer (qFlipper sends "start_rpc_session\r") */
    char cli_buf[64];
    size_t cli_len;

    /* RX scratch buffer */
    uint8_t rx_buf[USB_RPC_RX_CHUNK_SIZE];
} UsbRpcSrv;

/* Single-instance background bridge. */
static UsbRpcSrv* s_bridge = NULL;
static QflipperBridgeAutoOffCallback s_auto_off_cb = NULL;
static void* s_auto_off_ctx = NULL;

#if QFLIPPER_HAVE_COMPOSITE

/* ═════════════════════════════════════════════════════════════════════
 * QFDIAG — opt-in diagnostics for the qFlipper USB/RPC transfer path.
 * OFF by default; enable with CONFIG_FURI_QFDIAG (menuconfig "qFlipper / RPC
 * diagnostics", see furi_hal_qfdiag.h). With it off, qd_install() allocates
 * nothing and hooks nothing, and every qd_* call below is a no-op. Kept in the
 * tree on purpose: it found the root causes of QF-2/QF-3/QF-4.
 *
 * In qFlipper mode the USB PHY belongs to the OTG composite, so the
 * USB-Serial-JTAG console (where ESP_LOG goes) is dead and nothing the
 * firmware logs can be seen. Capture every ESP_LOG line into a RAM ring and
 * write it to the SD card when the host disconnects, plus a once-per-second
 * pipeline snapshot:  USB RX FIFO -> bridge -> RPC stream -> worker/handler ->
 * storage -> FatFs diskio (SPI bus lock vs card transaction) -> USB TX.
 * The file is then downloaded with qFlipper. Logging only — no behaviour
 * change.
 * ═════════════════════════════════════════════════════════════════════ */
#define QD_TAG        "QD"
#define QD_RING_SIZE  (256 * 1024)
#define QD_LINE_MAX   200
#define QD_STALL_MS   2000
#define QD_DUMP_IN_STALL_MS 20000
/* One file, appended per session, so qT-Embed's automatic probe session after
 * a reconnect cannot overwrite the dump of the session that failed. */
#define QD_FILE      "/ext/qfdiag.log"
#define QD_FILE_MAX  (768 * 1024)

static char* qd_ring;
static size_t qd_ring_size;
static size_t qd_head;
static bool qd_wrapped;
static volatile bool qd_paused;
static portMUX_TYPE qd_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t qd_prev_vprintf;

/* Pipeline counters — touched by the bridge thread only. */
static uint32_t qd_rx_total, qd_fed_total, qd_rx_prev, qd_fed_prev;
static uint32_t qd_last_rx_ms, qd_last_stats_ms;
static uint32_t qd_bw_since_ms; /* != 0 while drain_rx waits for RPC stream space */
static bool qd_session_stalled;
static bool qd_dumped_this_stall;

static int qd_vprintf(const char* fmt, va_list args) {
    if(qd_ring && !qd_paused) {
        char line[QD_LINE_MAX];
        va_list copy;
        va_copy(copy, args);
        const int n = vsnprintf(line, sizeof(line), fmt, copy);
        va_end(copy);
        if(n > 0) {
            size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
            if((size_t)n >= sizeof(line)) line[len - 1] = '\n'; /* truncated */
            portENTER_CRITICAL(&qd_mux);
            size_t first = qd_ring_size - qd_head;
            if(first > len) first = len;
            memcpy(qd_ring + qd_head, line, first);
            if(len > first) memcpy(qd_ring, line + first, len - first);
            qd_head += len;
            if(qd_head >= qd_ring_size) {
                qd_head -= qd_ring_size;
                qd_wrapped = true;
            }
            portEXIT_CRITICAL(&qd_mux);
        }
    }
    return qd_prev_vprintf ? qd_prev_vprintf(fmt, args) : 0;
}

static void qd_install(void) {
    if(!QFDIAG_ENABLED || qd_ring) return;
    size_t size = QD_RING_SIZE;
    char* ring = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!ring) {
        size = 16 * 1024;
        ring = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if(!ring) return;
    qd_ring_size = size;
    qd_head = 0;
    qd_wrapped = false;
    qd_ring = ring;
    qd_prev_vprintf = esp_log_set_vprintf(qd_vprintf);
    ESP_LOGI(
        QD_TAG,
        "qfdiag capture on (%u KB ring), boot reset=%d (esp_reset_reason_t)",
        (unsigned)(size / 1024),
        (int)esp_reset_reason());
}

/* Write the ring to the SD card. Capture is paused meanwhile so the ring is a
 * consistent snapshot and the dump's own storage logs do not overwrite it. */
static void qd_dump(bool stalled, const char* reason) {
    if(!qd_ring) return;
    static uint32_t dump_no;
    dump_no++;
    ESP_LOGI(QD_TAG, "dump #%lu (%s) -> %s", (unsigned long)dump_no, reason, QD_FILE);
    qd_paused = true;
    portENTER_CRITICAL(&qd_mux);
    const size_t head = qd_head;
    const bool wrapped = qd_wrapped;
    portEXIT_CRITICAL(&qd_mux);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool open = storage_file_open(file, QD_FILE, FSAM_WRITE, FSOM_OPEN_APPEND);
    if(open && storage_file_size(file) > QD_FILE_MAX) {
        storage_file_close(file);
        open = storage_file_open(file, QD_FILE, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    }
    if(open) {
        char hdr[160];
        const int hl = snprintf(
            hdr,
            sizeof(hdr),
            "\n\n=== qfdiag dump #%lu uptime=%lu ms boot_reset=%d stalled=%d reason=%s ===\n",
            (unsigned long)dump_no,
            (unsigned long)furi_get_tick(),
            (int)esp_reset_reason(),
            stalled,
            reason);
        if(hl > 0) storage_file_write(file, hdr, (size_t)hl);
        if(wrapped) storage_file_write(file, qd_ring + head, qd_ring_size - head);
        storage_file_write(file, qd_ring, head);
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    /* Start the next session's capture from an empty ring so each dump holds
     * only what happened since the previous one. */
    portENTER_CRITICAL(&qd_mux);
    qd_head = 0;
    qd_wrapped = false;
    portEXIT_CRITICAL(&qd_mux);
    qd_paused = false;
}

static void qd_stats(UsbRpcSrv* srv, const char* where) {
    if(!QFDIAG_ENABLED) return;
    const uint32_t now = furi_get_tick();
    FuriHalSdDebug sd;
    furi_hal_sd_debug_get(&sd);
    uint32_t htag = 0, hms = 0;
    rpc_debug_get_handler(&htag, &hms);
    const uint32_t bw = qd_bw_since_ms ? now - qd_bw_since_ms : 0;
    const uint32_t txms = furi_hal_cdc_tx_inflight_ms();
    const size_t rpc_free = srv->session_open ? rpc_session_get_available_size(srv->session) : 0;
    ESP_LOGI(
        QD_TAG,
        "%s rx+%lu fed+%lu fifo=%lu rpcfree=%u bw=%lu rxago=%lu tx=%lu",
        where,
        (unsigned long)(qd_rx_total - qd_rx_prev),
        (unsigned long)(qd_fed_total - qd_fed_prev),
        (unsigned long)furi_hal_cdc_rx_available(USB_RPC_CDC_ITF),
        (unsigned)rpc_free,
        (unsigned long)bw,
        (unsigned long)(qd_last_rx_ms ? now - qd_last_rx_ms : 0),
        (unsigned long)txms);
    static uint32_t ws_prev, wb_prev, rs_prev, rb_prev;
    ESP_LOGI(
        QD_TAG,
        "%s h=%lu/%lums sd op=%u %lums lockw=%lums | wsect+%lu wbusy+%lums rsect+%lu rbusy+%lums | err=%lu(%ld) maxw=%lu maxlw=%lu",
        where,
        (unsigned long)htag,
        (unsigned long)hms,
        (unsigned)sd.inflight_op,
        (unsigned long)sd.inflight_ms,
        (unsigned long)sd.lockwait_ms,
        (unsigned long)(sd.write_sectors - ws_prev),
        (unsigned long)(sd.write_busy_ms - wb_prev),
        (unsigned long)(sd.read_sectors - rs_prev),
        (unsigned long)(sd.read_busy_ms - rb_prev),
        (unsigned long)sd.errors,
        (long)sd.last_err,
        (unsigned long)sd.max_write_ms,
        (unsigned long)sd.max_lockwait_ms);
    ws_prev = sd.write_sectors;
    wb_prev = sd.write_busy_ms;
    rs_prev = sd.read_sectors;
    rb_prev = sd.read_busy_ms;
    qd_rx_prev = qd_rx_total;
    qd_fed_prev = qd_fed_total;
    qd_last_stats_ms = now;
    if(bw >= QD_STALL_MS || hms >= QD_STALL_MS || sd.inflight_ms >= QD_STALL_MS ||
       txms >= QD_STALL_MS) {
        qd_session_stalled = true;
    }
}
/* ═══════════════════════════ end QFDIAG ═══════════════════════════ */

/* ─────────────────────────────────────────────────────────────────────
 * CDC callbacks (from furi_hal_usb_cdc).
 *
 * These run on the TinyUSB task. We hand events to the bridge thread
 * via the message queue and return quickly.
 * ───────────────────────────────────────────────────────────────────── */

static void usb_rpc_post_event(UsbRpcSrv* srv, UsbRpcEvent ev) {
    furi_message_queue_put(srv->event_q, &ev, 0);
}

static void usb_rpc_cdc_tx_done(void* ctx) {
    /* The bridge transmits synchronously inside furi_hal_cdc_send(), so it has no
     * use for TX-complete notifications. Posting them only consumed slots in the
     * 16-deep event queue, and because RxAvailable is posted non-blocking (dropped
     * when the queue is full) that pressure could evict an RxAvailable and strand
     * inbound bytes in the CDC FIFO mid-transfer — a stalled upload that qFlipper
     * reports as "connection lost". Ignore the event. */
    UNUSED(ctx);
}

static void usb_rpc_cdc_rx(void* ctx) {
    usb_rpc_post_event(ctx, UsbRpcEventRxAvailable);
}

static void usb_rpc_cdc_state(void* ctx, CdcState state) {
    UsbRpcSrv* srv = ctx;
    if(state == CdcStateDisconnected) {
        usb_rpc_post_event(srv, UsbRpcEventDisconnected);
    }
    /* Connected via DTR edge in ctrl_line callback below */
}

static void usb_rpc_cdc_ctrl_line(void* ctx, CdcCtrlLine ctrl) {
    UsbRpcSrv* srv = ctx;
    if(ctrl & CdcCtrlLineDTR) {
        usb_rpc_post_event(srv, UsbRpcEventConnected);
    } else {
        usb_rpc_post_event(srv, UsbRpcEventDisconnected);
    }
}

static CdcCallbacks usb_rpc_cdc_callbacks = {
    .tx_ep_callback = usb_rpc_cdc_tx_done,
    .rx_ep_callback = usb_rpc_cdc_rx,
    .state_callback = usb_rpc_cdc_state,
    .ctrl_line_callback = usb_rpc_cdc_ctrl_line,
    .config_callback = NULL,
};

static bool qflipper_bridge_is_exiting(const UsbRpcSrv* srv);

/* ─────────────────────────────────────────────────────────────────────
 * RPC -> CDC (outbound)
 * ───────────────────────────────────────────────────────────────────── */

static void usb_rpc_send_bytes(void* ctx, uint8_t* bytes, size_t len) {
    (void)ctx;
    while(len > 0) {
        uint16_t chunk = (len > 0xFFFF) ? 0xFFFF : (uint16_t)len;
        furi_hal_cdc_send(USB_RPC_CDC_ITF, bytes, chunk);
        bytes += chunk;
        len -= chunk;
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * Session lifecycle
 * ───────────────────────────────────────────────────────────────────── */

static void usb_rpc_session_open(UsbRpcSrv* srv) {
    if(srv->session_open) return;

    srv->session = rpc_session_open(srv->rpc, RpcOwnerUsb);
    if(!srv->session) {
        FURI_LOG_E(TAG, "rpc_session_open failed");
        return;
    }
    rpc_session_set_context(srv->session, srv);
    rpc_session_set_send_bytes_callback(srv->session, usb_rpc_send_bytes);
    srv->session_open = true;
    FURI_LOG_I(TAG, "RPC session opened");
    if(QFDIAG_ENABLED) { /* QFDIAG */
        qd_session_stalled = false;
        qd_dumped_this_stall = false;
        ESP_LOGI(QD_TAG, "RPC session opened");
    }
}

static void usb_rpc_session_close(UsbRpcSrv* srv) {
    if(!srv->session_open) return;

    rpc_session_close(srv->session);
    srv->session = NULL;
    srv->session_open = false;
    FURI_LOG_I(TAG, "RPC session closed");
}

static size_t usb_rpc_feed(RpcSession* session, const uint8_t* data, size_t length, UsbRpcSrv* srv) {
    size_t total_fed = 0;
    while(total_fed < length && !qflipper_bridge_is_exiting(srv)) {
        size_t fed = rpc_session_feed(
            session,
            data + total_fed,
            length - total_fed,
            furi_ms_to_ticks(20));
        total_fed += fed;
    }
    return total_fed;
}

static void usb_rpc_drain_rx(UsbRpcSrv* srv) {
    if(!srv->session_open) {
        uint8_t scratch[64];
        while(furi_hal_cdc_receive(USB_RPC_CDC_ITF, scratch, sizeof(scratch)) > 0) {
        }
        return;
    }

    /* Never read from the CDC FIFO unless we have downstream room for the whole
     * chunk we're about to consume. If the RPC stream is saturated we sit-and-
     * wait here so the TinyUSB RX FIFO fills up and USB-NAKs the host — the only
     * flow-control mechanism over USB-CDC. */
    while(true) {
        if(qflipper_bridge_is_exiting(srv)) return;
        size_t avail = rpc_session_get_available_size(srv->session);
        if(avail == 0) {
            if(QFDIAG_ENABLED) {
                /* QFDIAG: the RPC stream is full — the worker is not draining. */
                const uint32_t now = furi_get_tick();
                if(!qd_bw_since_ms) qd_bw_since_ms = now ? now : 1;
                if(now - qd_last_stats_ms >= 1000) qd_stats(srv, "BW");
                if(!qd_dumped_this_stall && now - qd_bw_since_ms >= QD_DUMP_IN_STALL_MS) {
                    /* Stuck long enough that the host has surely given up; if the
                     * worker never recovers we would otherwise never dump. */
                    qd_dumped_this_stall = true;
                    qd_dump(true, "stuck-in-busywait");
                }
            }
            furi_delay_ms(1);
            if(!srv->session_open || qflipper_bridge_is_exiting(srv)) return;
            continue;
        }
        if(QFDIAG_ENABLED && qd_bw_since_ms) { /* QFDIAG */
            const uint32_t waited = furi_get_tick() - qd_bw_since_ms;
            if(waited >= 200) ESP_LOGW(QD_TAG, "rpc stream was full for %lu ms", (unsigned long)waited);
            qd_bw_since_ms = 0;
        }

        size_t to_read = avail < sizeof(srv->rx_buf) ? avail : sizeof(srv->rx_buf);
        int32_t got = furi_hal_cdc_receive(USB_RPC_CDC_ITF, srv->rx_buf, to_read);
        if(got <= 0) {
            break;
        }
        if(QFDIAG_ENABLED) { /* QFDIAG */
            qd_rx_total += (uint32_t)got;
            qd_last_rx_ms = furi_get_tick();
        }

        size_t fed = usb_rpc_feed(srv->session, srv->rx_buf, (size_t)got, srv);
        if(QFDIAG_ENABLED) qd_fed_total += (uint32_t)fed; /* QFDIAG */
        if(fed != (size_t)got) {
            if(!qflipper_bridge_is_exiting(srv)) {
                FURI_LOG_E(TAG, "rpc_session_feed underran: %zu/%ld", fed, got);
            }
            break;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * CLI handshake — qFlipper drives the text CLI before piping protobuf.
 * We emulate just enough to satisfy the handshake, then flip into RPC mode.
 * ───────────────────────────────────────────────────────────────────── */

#define QFLIPPER_CLI_PROMPT  "\r\n\r\n>: "
#define QFLIPPER_RPC_COMMAND "start_rpc_session"

static void qflipper_cli_send_prompt(void) {
    furi_hal_cdc_send(
        USB_RPC_CDC_ITF, (uint8_t*)QFLIPPER_CLI_PROMPT, strlen(QFLIPPER_CLI_PROMPT));
}

static void qflipper_cli_rx(UsbRpcSrv* srv) {
    uint8_t buf[64];
    int32_t got;
    while((got = furi_hal_cdc_receive(USB_RPC_CDC_ITF, buf, sizeof(buf))) > 0) {
        for(int32_t i = 0; i < got; i++) {
            char c = (char)buf[i];

            if(c == '\r' || c == '\n') {
                srv->cli_buf[srv->cli_len] = '\0';

                if(strstr(srv->cli_buf, QFLIPPER_RPC_COMMAND) != NULL) {
                    furi_hal_cdc_send(
                        USB_RPC_CDC_ITF,
                        (uint8_t*)(QFLIPPER_RPC_COMMAND "\r\n"),
                        strlen(QFLIPPER_RPC_COMMAND "\r\n"));

                    srv->cli_len = 0;
                    srv->rpc_mode = true;
                    usb_rpc_session_open(srv);

                    if(srv->session_open && (i + 1) < got) {
                        usb_rpc_feed(
                            srv->session, &buf[i + 1], (size_t)(got - i - 1), srv);
                    }
                    return;
                }

                srv->cli_len = 0;
                qflipper_cli_send_prompt();
            } else if(srv->cli_len < sizeof(srv->cli_buf) - 1) {
                srv->cli_buf[srv->cli_len++] = c;
            }
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * Bridge thread
 * ───────────────────────────────────────────────────────────────────── */

static void qflipper_bridge_run(UsbRpcSrv* srv) {
    UsbRpcEvent ev;
    while(true) {
        /* Pick the event wait:
         *  - host gone after a session: wait the auto-off grace period, then ask
         *    the desktop to leave qFlipper mode;
         *  - connected and piping RPC: wait only USB_RPC_RX_POLL_MS so the FIFO is
         *    re-drained even if an RxAvailable event was dropped (see above);
         *  - otherwise (idle / CLI handshake): block until an event arrives. */
        uint32_t wait;
        if(srv->auto_off_armed) {
            wait = furi_ms_to_ticks(QFLIPPER_BRIDGE_AUTO_OFF_MS);
        } else if(srv->connected && srv->rpc_mode) {
            wait = furi_ms_to_ticks(USB_RPC_RX_POLL_MS);
        } else {
            wait = FuriWaitForever;
        }
        FuriStatus st = furi_message_queue_get(srv->event_q, &ev, wait);
        if(QFDIAG_ENABLED && srv->rpc_mode && furi_get_tick() - qd_last_stats_ms >= 1000) { /* QFDIAG */
            qd_stats(srv, "ST");
        }
        if(st == FuriStatusErrorTimeout) {
            if(srv->auto_off_armed && !srv->connected && !qflipper_bridge_is_exiting(srv)) {
                srv->auto_off_armed = false;
                FURI_LOG_I(TAG, "host gone for %u ms — leaving qFlipper mode", (unsigned)QFLIPPER_BRIDGE_AUTO_OFF_MS);
                if(s_auto_off_cb) s_auto_off_cb(s_auto_off_ctx);
            } else if(srv->rpc_mode && !qflipper_bridge_is_exiting(srv)) {
                /* Safety net: drain any bytes left by a dropped RxAvailable event. */
                usb_rpc_drain_rx(srv);
            }
            continue;
        }
        if(st != FuriStatusOk) {
            continue;
        }

        switch(ev) {
        case UsbRpcEventExit:
            return;

        case UsbRpcEventConnected:
            srv->auto_off_armed = false;
            if(srv->connected) break;
            srv->connected = true;
            srv->rpc_mode = false;
            srv->cli_len = 0;
            FURI_LOG_I(TAG, "DTR up — entering CLI, sending prompt");
            if(QFDIAG_ENABLED) ESP_LOGI(QD_TAG, "DTR up"); /* QFDIAG */
            qflipper_cli_send_prompt();
            break;

        case UsbRpcEventDisconnected: {
            if(!srv->connected) break;
            /* QFDIAG: snapshot the pipeline at the moment the host gave up. */
            const bool qd_had_session = srv->session_open;
            if(qd_had_session) qd_stats(srv, "DISC");
            srv->connected = false;
            srv->rpc_mode = false;
            srv->cli_len = 0;
            srv->auto_off_armed = true;
            FURI_LOG_I(TAG, "DTR down — closing RPC session");
            if(QFDIAG_ENABLED) ESP_LOGI(QD_TAG, "DTR down — closing RPC session");
            usb_rpc_session_close(srv);
            if(qd_had_session) qd_dump(qd_session_stalled, "disconnect");
            break;
        }

        case UsbRpcEventRxAvailable:
            if(srv->rpc_mode) {
                usb_rpc_drain_rx(srv);
            } else {
                qflipper_cli_rx(srv);
            }
            break;

        case UsbRpcEventTxComplete:
            break;
        }
    }
}

/* The thread only runs the event loop. The composite is installed before the
 * thread starts (start()) and torn down after it joins (stop()), both on the
 * caller's (desktop) thread. The exiting flag lets the loop bail out of a
 * blocking RX drain so join() always completes promptly. */
static int32_t qflipper_bridge_thread(void* ctx) {
    UsbRpcSrv* srv = ctx;
    qflipper_bridge_run(srv);
    return 0;
}

#endif /* QFLIPPER_HAVE_COMPOSITE */

/* ─────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────────── */

#if QFLIPPER_HAVE_COMPOSITE
static FuriMutex* s_bridge_mutex;
static bool s_resume_after_u2f;

static bool qflipper_bridge_lock(void) {
    static uint32_t mutex_init_state;
    while(__atomic_load_n(&mutex_init_state, __ATOMIC_ACQUIRE) != 2) {
        uint32_t expected = 0;
        if(__atomic_compare_exchange_n(
               &mutex_init_state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            s_bridge_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
            if(!s_bridge_mutex) {
                __atomic_store_n(&mutex_init_state, 0, __ATOMIC_RELEASE);
                FURI_LOG_E(TAG, "Cannot allocate qFlipper bridge mutex");
                return false;
            }
            __atomic_store_n(&mutex_init_state, 2, __ATOMIC_RELEASE);
            break;
        }
        furi_delay_ms(1);
    }
    furi_mutex_acquire(s_bridge_mutex, FuriWaitForever);
    return true;
}

static bool qflipper_bridge_is_exiting(const UsbRpcSrv* srv) {
    return __atomic_load_n(&srv->exiting, __ATOMIC_ACQUIRE);
}

static void qflipper_bridge_request_exit(UsbRpcSrv* srv) {
    __atomic_store_n(&srv->exiting, true, __ATOMIC_RELEASE);
}

static bool qflipper_bridge_start_locked(void);
static void qflipper_bridge_stop_locked(void);

static bool qflipper_bridge_u2f_preemption(bool acquire, void* context) {
    UNUSED(context);

    if(!qflipper_bridge_lock()) return false;

    bool result = true;
    if(acquire) {
        if(!s_bridge || furi_hal_usb_msc_is_active()) {
            result = false;
        } else {
            qflipper_bridge_stop_locked();
            if(!furi_hal_usb_composite_uninstall()) {
                FURI_LOG_E(TAG, "Cannot release qFlipper USB for U2F");
                if(!qflipper_bridge_start_locked()) {
                    FURI_LOG_E(TAG, "Failed to restore qFlipper after USB handoff failure");
                }
                result = false;
            } else {
                s_resume_after_u2f = true;
            }
        }
    } else if(s_resume_after_u2f) {
        s_resume_after_u2f = false;
        if(!qflipper_bridge_start_locked()) {
            FURI_LOG_E(TAG, "Failed to resume qFlipper after U2F released USB");
            result = false;
        }
    }

    furi_mutex_release(s_bridge_mutex);
    return result;
}

static bool qflipper_bridge_start_locked(void) {
    if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagLock) &&
       !momentum_settings_get_bool(MomentumSettingsBoolAllowLockedRpcUsb)) {
        FURI_LOG_W(TAG, "USB RPC bridge is disabled while locked");
        return false;
    }
    if(s_bridge) return true; /* already active */
    /* USB-Storage owns the composite while MSC exposes the SD card. Starting
     * here would call furi_hal_usb_reinit() and force a re-enumeration in the
     * middle of host writes (e.g. the OTA-resume timer firing during MSC).
     * Same rule as the U2F preemption path, which also refuses during MSC. */
    if(furi_hal_usb_msc_is_active()) {
        FURI_LOG_W(TAG, "USB-Storage is active; not starting the qFlipper bridge");
        return false;
    }

    UsbRpcSrv* srv = malloc(sizeof(UsbRpcSrv));
    if(!srv) {
        FURI_LOG_E(TAG, "Cannot allocate qFlipper bridge state");
        return false;
    }
    srv->event_q = furi_message_queue_alloc(16, sizeof(UsbRpcEvent));
    if(!srv->event_q) {
        FURI_LOG_E(TAG, "Cannot allocate qFlipper bridge event queue");
        free(srv);
        return false;
    }
    srv->session = NULL;
    srv->connected = false;
    srv->session_open = false;
    srv->rpc_mode = false;
    srv->auto_off_armed = false;
    srv->exiting = false;
    srv->cli_len = 0;

    srv->rpc = furi_record_open(RECORD_RPC);
    furi_check(srv->rpc);

    srv->thread = furi_thread_alloc_ex("QflipperBridge", 8192, qflipper_bridge_thread, srv);
    if(!srv->thread) {
        FURI_LOG_E(TAG, "Cannot allocate qFlipper bridge thread");
        furi_record_close(RECORD_RPC);
        furi_message_queue_free(srv->event_q);
        free(srv);
        return false;
    }

    /* Install the composite as a real Flipper Zero (0483:5740). Idempotent if
     * another consumer (USB-Storage) already installed it. */
    if(!furi_hal_usb_composite_install(0, 0, NULL, NULL)) {
        FURI_LOG_E(TAG, "composite_install failed");
        furi_thread_free(srv->thread);
        furi_record_close(RECORD_RPC);
        furi_message_queue_free(srv->event_q);
        free(srv);
        return false;
    }

    /* set_callbacks fires state/ctrl_line immediately with the current line
     * state, so if DTR is already high this seeds a Connected event into the
     * queue (drained once the thread starts). */
    furi_hal_cdc_set_callbacks(USB_RPC_CDC_ITF, &usb_rpc_cdc_callbacks, srv);
    furi_hal_usb_reinit();

    s_bridge = srv;
    furi_hal_usb_set_u2f_preemption_callback(qflipper_bridge_u2f_preemption, NULL);
    qd_install(); /* QFDIAG */
    furi_thread_start(srv->thread);

    return true;
}

static void qflipper_bridge_stop_locked(void) {
    UsbRpcSrv* srv = s_bridge;
    if(!srv) return;
    s_bridge = NULL;

    /* Wake the loop out of any blocking RX drain, then ask it to exit. */
    qflipper_bridge_request_exit(srv);
    UsbRpcEvent ev = UsbRpcEventExit;
    furi_message_queue_put(srv->event_q, &ev, FuriWaitForever);

    furi_thread_join(srv->thread);
    furi_thread_free(srv->thread);

    /* Detach the RPC bridge only and LEAVE the composite installed. This is the
     * shared mutual-exclusion hook (USB-Storage calls it before reusing the
     * composite for MSC), so tearing the composite down HERE would churn it.
     * The user-facing "Disable qFlipper" path uninstalls the composite right
     * after this (see the file header) to route the PHY back to USJ. */
    furi_hal_cdc_set_callbacks(USB_RPC_CDC_ITF, NULL, NULL);
    usb_rpc_session_close(srv);
    furi_record_close(RECORD_RPC);

    furi_message_queue_free(srv->event_q);
    free(srv);
}
#endif

bool qflipper_bridge_start(void) {
#if !QFLIPPER_HAVE_COMPOSITE
    return false;
#else
    /* Lock order is always USB HAL mutex -> bridge mutex. A USB lease or U2F
     * switch holds the USB mutex while it calls qflipper_bridge_u2f_preemption(),
     * which then takes the bridge mutex. start_locked() needs the USB mutex too
     * (composite install, reinit, callback registration); taking it here first
     * keeps both paths in the same order instead of bridge -> USB, which could
     * deadlock against a concurrent lease. The USB mutex is recursive, so the
     * nested HAL calls inside start_locked() re-enter it. Not used by stop():
     * stop_locked() joins the bridge thread and takes no USB mutex. */
    if(!furi_hal_usb_operation_start(NULL)) {
        FURI_LOG_W(TAG, "USB is leased or locked; not starting the qFlipper bridge");
        return false;
    }
    bool result = false;
    if(qflipper_bridge_lock()) {
        result = qflipper_bridge_start_locked();
        furi_mutex_release(s_bridge_mutex);
    }
    furi_hal_usb_operation_finish();
    return result;
#endif
}

void qflipper_bridge_stop(void) {
#if QFLIPPER_HAVE_COMPOSITE
    if(!qflipper_bridge_lock()) return;
    qflipper_bridge_stop_locked();
    furi_mutex_release(s_bridge_mutex);
#endif
}

bool qflipper_bridge_is_active(void) {
#if QFLIPPER_HAVE_COMPOSITE
    if(!qflipper_bridge_lock()) return false;
    bool active = s_bridge != NULL;
    furi_mutex_release(s_bridge_mutex);
    return active;
#else
    return s_bridge != NULL;
#endif
}

void qflipper_bridge_set_auto_off_callback(QflipperBridgeAutoOffCallback callback, void* context) {
    s_auto_off_cb = callback;
    s_auto_off_ctx = context;
}
