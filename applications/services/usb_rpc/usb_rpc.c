/**
 * usb_rpc service: bridges TinyUSB CDC-ACM to Flipper RPC.
 *
 * Replaces cli_vcp + start_rpc_session for the ESP32 port. Once a host opens
 * the CDC port (DTR goes high), this service opens an RpcSession with
 * RpcOwnerUsb and pipes bytes back and forth. No CLI shell, no banner, no
 * mode-switch handshake — the host can speak protobuf RPC immediately.
 */

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_cdc.h>
#include <rpc/rpc.h>

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2
#include "furi_hal_usb_tinyusb_composite.h"
#define USB_RPC_HAVE_COMPOSITE 1
#else
#define USB_RPC_HAVE_COMPOSITE 0
#endif

#define TAG "UsbRpc"

#define USB_RPC_CDC_ITF       0
/* Larger drain chunks shorten the race window between the avail-check and
 * the FIFO read in usb_rpc_drain_rx — fewer iterations, fewer chances for
 * the host to slip a byte into the FIFO that we'd then drop. */
#define USB_RPC_RX_CHUNK_SIZE 2048

/* Internal events */
typedef enum {
    UsbRpcEventConnected,    /* DTR went high */
    UsbRpcEventDisconnected, /* DTR went low or USB disconnect */
    UsbRpcEventRxAvailable,  /* CDC RX bytes ready */
    UsbRpcEventTxComplete,   /* CDC TX done */
} UsbRpcEvent;

typedef struct {
    FuriThread* thread;
    FuriMessageQueue* event_q;

    Rpc* rpc;
    RpcSession* session;

    bool connected;     /* DTR state */
    bool session_open;  /* RPC session active */

    /* RX scratch buffer */
    uint8_t rx_buf[USB_RPC_RX_CHUNK_SIZE];
} UsbRpcSrv;

/* ─────────────────────────────────────────────────────────────────────
 * CDC callbacks (from furi_hal_usb_cdc).
 *
 * These run on the TinyUSB task. We hand events to the service thread
 * via the message queue and return quickly.
 * ───────────────────────────────────────────────────────────────────── */

static void usb_rpc_post_event(UsbRpcSrv* srv, UsbRpcEvent ev) {
    /* Queue may be full transiently — drop is acceptable for RX/TX events
     * (the service polls anyway), but Connected/Disconnected we want. */
    furi_message_queue_put(srv->event_q, &ev, 0);
}

static void usb_rpc_cdc_tx_done(void* ctx) {
    usb_rpc_post_event(ctx, UsbRpcEventTxComplete);
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

/* ─────────────────────────────────────────────────────────────────────
 * RPC -> CDC (outbound)
 * ───────────────────────────────────────────────────────────────────── */

static void usb_rpc_send_bytes(void* ctx, uint8_t* bytes, size_t len) {
    (void)ctx;
    /* furi_hal_cdc_send queues the bytes in the TinyUSB write buffer and
     * triggers an async flush. The TX-complete callback fires when the
     * transfer drains; we don't need to block here. */
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
}

static void usb_rpc_session_close(UsbRpcSrv* srv) {
    if(!srv->session_open) return;

    rpc_session_close(srv->session);
    srv->session = NULL;
    srv->session_open = false;
    FURI_LOG_I(TAG, "RPC session closed");
}

static void usb_rpc_drain_rx(UsbRpcSrv* srv) {
    if(!srv->session_open) {
        /* No session yet: just drain to avoid backpressure */
        uint8_t scratch[64];
        while(furi_hal_cdc_receive(USB_RPC_CDC_ITF, scratch, sizeof(scratch)) > 0) {
        }
        return;
    }

    /* Mirrors the STM32 cli_vcp pattern: never read from the CDC FIFO unless
     * we have downstream room for the whole chunk we're about to consume.
     * If the RPC stream is saturated we sit-and-wait inside this loop —
     * that way the TinyUSB RX FIFO fills up and USB-NAKs the host, which is
     * the only flow-control mechanism available over USB-CDC. Returning
     * here would let the next host write trigger another RX event whose
     * bytes might race past us into a still-full stream. */
    while(true) {
        size_t avail = rpc_session_get_available_size(srv->session);
        if(avail == 0) {
            /* RPC consumer hasn't drained yet. Yield briefly and retry. */
            furi_delay_ms(1);
            if(!srv->session_open) return;
            continue;
        }

        size_t to_read = avail < sizeof(srv->rx_buf) ? avail : sizeof(srv->rx_buf);
        int32_t got = furi_hal_cdc_receive(USB_RPC_CDC_ITF, srv->rx_buf, to_read);
        if(got <= 0) {
            /* TinyUSB FIFO is empty — no more pending host bytes. Done. */
            break;
        }

        /* We sized `to_read` to fit the stream, so feed() must consume it
         * all in one go (and quickly). If it underruns, the session was
         * terminated by the peer; bail out cleanly. */
        size_t fed = rpc_session_feed(
            srv->session, srv->rx_buf, (size_t)got, FuriWaitForever);
        if(fed != (size_t)got) {
            FURI_LOG_E(TAG, "rpc_session_feed underran: %zu/%ld", fed, got);
            break;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * Service thread
 * ───────────────────────────────────────────────────────────────────── */

static int32_t usb_rpc_thread(void* ctx) {
    UsbRpcSrv* srv = ctx;

#if USB_RPC_HAVE_COMPOSITE
    FURI_LOG_I(TAG, "Installing TinyUSB Composite (HID + CDC)...");
    if(!furi_hal_usb_composite_install(0, 0, NULL, NULL)) {
        FURI_LOG_E(TAG, "composite_install failed; service exiting");
        return 0;
    }
    FURI_LOG_I(TAG, "Composite installed");
#else
    FURI_LOG_I(TAG, "USB-OTG unsupported on this target; usb_rpc inactive");
    return 0;
#endif

    /* Wire CDC events to ourselves. furi_hal_cdc_set_callbacks fires the
     * state/ctrl_line/config callbacks immediately with the current state,
     * which seeds our event queue if DTR was already high. */
    furi_hal_cdc_set_callbacks(USB_RPC_CDC_ITF, &usb_rpc_cdc_callbacks, srv);

    UsbRpcEvent ev;
    while(true) {
        if(furi_message_queue_get(srv->event_q, &ev, FuriWaitForever) != FuriStatusOk) {
            continue;
        }

        switch(ev) {
        case UsbRpcEventConnected:
            if(srv->connected) break;
            srv->connected = true;
            FURI_LOG_I(TAG, "DTR up — opening RPC session");
            usb_rpc_session_open(srv);
            /* Drain any RX bytes that piled up before we opened. */
            usb_rpc_drain_rx(srv);
            break;

        case UsbRpcEventDisconnected:
            if(!srv->connected) break;
            srv->connected = false;
            FURI_LOG_I(TAG, "DTR down — closing RPC session");
            usb_rpc_session_close(srv);
            break;

        case UsbRpcEventRxAvailable:
            /* drain_rx now blocks internally on RPC backpressure until the
             * TinyUSB FIFO is fully drained, so a single call suffices. */
            usb_rpc_drain_rx(srv);
            break;

        case UsbRpcEventTxComplete:
            /* No-op: RPC's send_bytes_callback is fire-and-forget; TinyUSB
             * handles internal write-queue draining. We may use this in the
             * future for explicit flow control. */
            break;
        }
    }
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────
 * Service entry point
 * ───────────────────────────────────────────────────────────────────── */

int32_t usb_rpc_srv(void* p) {
    UNUSED(p);

    UsbRpcSrv* srv = malloc(sizeof(UsbRpcSrv));
    srv->event_q = furi_message_queue_alloc(16, sizeof(UsbRpcEvent));
    srv->rpc = furi_record_open(RECORD_RPC);
    srv->session = NULL;
    srv->connected = false;
    srv->session_open = false;

    /* Run the rest in this thread (we already have the 8 KiB stack). */
    return usb_rpc_thread(srv);
}
