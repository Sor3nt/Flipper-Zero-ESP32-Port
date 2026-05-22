/**
 * "qFlipper" — bridges TinyUSB CDC-ACM to Flipper RPC so the desktop qFlipper
 * app (and other protobuf-RPC hosts) can talk to the device.
 *
 * This is a main-menu app rather than a boot service on purpose: installing
 * the TinyUSB composite switches the ESP32-S3 internal USB PHY from
 * USB-Serial-JTAG to USB-OTG, which kills the serial/JTAG bridge that esptool
 * uses for flashing. So we only do it on demand — the user opens "qFlipper",
 * the composite comes up as VID/PID 0483:5740 (a real Flipper Zero), and an
 * RpcSession is piped over CDC. No CLI shell, no banner, no mode-switch
 * handshake — the host speaks protobuf RPC immediately.
 *
 * The user sees a full-screen status page and presses Back to leave. The
 * composite descriptor stays installed until the next reboot (it cannot be
 * torn down at runtime), so flashing/serial only return after a power cycle.
 *
 * Only the ESP32-S3 / S2 path has USB-OTG; the app is excluded from the
 * Waveshare C6 builds via fam_config.py.
 */

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_cdc.h>
#include <rpc/rpc.h>

#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/elements.h>
#include <input/input.h>

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2
#include "furi_hal_usb_tinyusb_composite.h"
#define QFLIPPER_HAVE_COMPOSITE 1
#else
#define QFLIPPER_HAVE_COMPOSITE 0
#endif

#define TAG "qFlipper"

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
    UsbRpcEventExit,         /* user pressed Back — leave the app */
} UsbRpcEvent;

typedef enum {
    QflipperStateInit,
    QflipperStateActive,
    QflipperStateError,
} QflipperState;

typedef struct {
    FuriMessageQueue* event_q;

    Rpc* rpc;
    RpcSession* session;

    bool connected;    /* DTR state */
    bool session_open; /* RPC session active */
    bool rpc_mode;     /* false = CLI handshake, true = piping protobuf */

    /* CLI handshake line buffer (qFlipper sends "start_rpc_session\r") */
    char cli_buf[64];
    size_t cli_len;

    /* GUI */
    QflipperState state;
    const char* error_msg;
    FuriMutex* draw_mutex;

    /* RX scratch buffer */
    uint8_t rx_buf[USB_RPC_RX_CHUNK_SIZE];
} UsbRpcSrv;

/* ─────────────────────────────────────────────────────────────────────
 * CDC callbacks (from furi_hal_usb_cdc).
 *
 * These run on the TinyUSB task. We hand events to the app thread
 * via the message queue and return quickly.
 * ───────────────────────────────────────────────────────────────────── */

static void usb_rpc_post_event(UsbRpcSrv* srv, UsbRpcEvent ev) {
    /* Queue may be full transiently — drop is acceptable for RX/TX events
     * (the app polls anyway), but Connected/Disconnected we want. */
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
        size_t fed = rpc_session_feed(srv->session, srv->rx_buf, (size_t)got, FuriWaitForever);
        if(fed != (size_t)got) {
            FURI_LOG_E(TAG, "rpc_session_feed underran: %zu/%ld", fed, got);
            break;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * CLI handshake
 *
 * qFlipper (and every other proper Flipper RPC client) does NOT speak
 * protobuf straight away. It first drives the text CLI exactly like the STM32
 * firmware exposes it:
 *   1) On DTR-up it reads the MOTD until the prompt token "\r\n\r\n>: ".
 *   2) It writes "start_rpc_session\r" and waits for that line to be echoed
 *      back terminated by "\r\n".
 *   3) Only then does it pipe protobuf.
 * We emulate just enough of that CLI to satisfy the handshake, then flip into
 * RPC mode and hand the byte stream to the RpcSession.
 * ───────────────────────────────────────────────────────────────────── */

#define QFLIPPER_CLI_PROMPT  "\r\n\r\n>: "
#define QFLIPPER_RPC_COMMAND "start_rpc_session"

static void qflipper_cli_send_prompt(void) {
    furi_hal_cdc_send(
        USB_RPC_CDC_ITF, (uint8_t*)QFLIPPER_CLI_PROMPT, strlen(QFLIPPER_CLI_PROMPT));
}

/* Consume CDC bytes while in CLI mode. Returns once "start_rpc_session" has
 * been seen — echoes it back terminated by CRLF, opens the RPC session and
 * switches the bridge into RPC mode (any trailing bytes are fed straight to
 * the session). */
static void qflipper_cli_rx(UsbRpcSrv* srv) {
    uint8_t buf[64];
    int32_t got;
    while((got = furi_hal_cdc_receive(USB_RPC_CDC_ITF, buf, sizeof(buf))) > 0) {
        for(int32_t i = 0; i < got; i++) {
            char c = (char)buf[i];

            if(c == '\r' || c == '\n') {
                srv->cli_buf[srv->cli_len] = '\0';

                if(strstr(srv->cli_buf, QFLIPPER_RPC_COMMAND) != NULL) {
                    /* Echo the command terminated by CRLF — StartRPCOperation
                     * waits for the stream to end with "...start_rpc_session\r\n". */
                    furi_hal_cdc_send(
                        USB_RPC_CDC_ITF,
                        (uint8_t*)(QFLIPPER_RPC_COMMAND "\r\n"),
                        strlen(QFLIPPER_RPC_COMMAND "\r\n"));

                    srv->cli_len = 0;
                    srv->rpc_mode = true;
                    usb_rpc_session_open(srv);

                    /* Bytes after the command in this same chunk are protobuf. */
                    if(srv->session_open && (i + 1) < got) {
                        rpc_session_feed(
                            srv->session, &buf[i + 1], (size_t)(got - i - 1), FuriWaitForever);
                    }
                    return;
                }

                /* Unknown line — reprint the prompt and keep waiting. */
                srv->cli_len = 0;
                qflipper_cli_send_prompt();
            } else if(srv->cli_len < sizeof(srv->cli_buf) - 1) {
                srv->cli_buf[srv->cli_len++] = c;
            }
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * GUI
 * ───────────────────────────────────────────────────────────────────── */

static void qflipper_draw_callback(Canvas* canvas, void* context) {
    UsbRpcSrv* srv = context;
    furi_mutex_acquire(srv->draw_mutex, FuriWaitForever);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "qFlipper");
    canvas_draw_line(canvas, 16, 13, 112, 13);

    canvas_set_font(canvas, FontSecondary);

    switch(srv->state) {
    case QflipperStateInit:
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Starting USB ...");
        break;
    case QflipperStateActive:
        canvas_draw_str_aligned(
            canvas, 64, 24, AlignCenter, AlignCenter, "Connect qFlipper on PC");
        canvas_set_font(canvas, FontBatteryPercent);
        canvas_draw_str_aligned(
            canvas, 64, 40, AlignCenter, AlignCenter, "Disconnect restores flash mode");
        canvas_set_font(canvas, FontSecondary);
        elements_button_left(canvas, "Disconnect");
        break;
    case QflipperStateError:
        canvas_draw_str_aligned(
            canvas, 64, 28, AlignCenter, AlignCenter, srv->error_msg ? srv->error_msg : "Error");
        elements_button_left(canvas, "Back");
        break;
    }

    furi_mutex_release(srv->draw_mutex);
}

static void qflipper_input_callback(InputEvent* event, void* context) {
    UsbRpcSrv* srv = context;
    if(event->type != InputTypeShort) return;
    if(event->key == InputKeyBack || event->key == InputKeyUp) {
        /* Guarantee delivery: the run loop only blocks on queue_get and drains
         * fast, so the queue won't stay full and we won't wedge the GUI. */
        UsbRpcEvent ev = UsbRpcEventExit;
        furi_message_queue_put(srv->event_q, &ev, FuriWaitForever);
    }
}

static void qflipper_set_state(UsbRpcSrv* srv, QflipperState state) {
    furi_mutex_acquire(srv->draw_mutex, FuriWaitForever);
    srv->state = state;
    furi_mutex_release(srv->draw_mutex);
}

/* Event loop. Returns when the user presses Back. */
static void qflipper_run(UsbRpcSrv* srv) {
    UsbRpcEvent ev;
    while(true) {
        if(furi_message_queue_get(srv->event_q, &ev, FuriWaitForever) != FuriStatusOk) {
            continue;
        }

        switch(ev) {
        case UsbRpcEventExit:
            return;

        case UsbRpcEventConnected:
            if(srv->connected) break;
            srv->connected = true;
            srv->rpc_mode = false;
            srv->cli_len = 0;
            FURI_LOG_I(TAG, "DTR up — entering CLI, sending prompt");
            /* Emit the MOTD prompt so qFlipper's SkipMOTD step completes. RPC
             * session is opened later, once the host issues start_rpc_session. */
            qflipper_cli_send_prompt();
            break;

        case UsbRpcEventDisconnected:
            if(!srv->connected) break;
            srv->connected = false;
            srv->rpc_mode = false;
            srv->cli_len = 0;
            FURI_LOG_I(TAG, "DTR down — closing RPC session");
            usb_rpc_session_close(srv);
            break;

        case UsbRpcEventRxAvailable:
            if(srv->rpc_mode) {
                /* drain_rx blocks internally on RPC backpressure until the
                 * TinyUSB FIFO is fully drained, so a single call suffices. */
                usb_rpc_drain_rx(srv);
            } else {
                qflipper_cli_rx(srv);
            }
            break;

        case UsbRpcEventTxComplete:
            /* No-op: RPC's send_bytes_callback is fire-and-forget; TinyUSB
             * handles internal write-queue draining. */
            break;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * App entry point
 * ───────────────────────────────────────────────────────────────────── */

int32_t qflipper_app(void* p) {
    UNUSED(p);

    UsbRpcSrv* srv = malloc(sizeof(UsbRpcSrv));
    srv->event_q = furi_message_queue_alloc(16, sizeof(UsbRpcEvent));
    srv->rpc = NULL;
    srv->session = NULL;
    srv->connected = false;
    srv->session_open = false;
    srv->state = QflipperStateInit;
    srv->error_msg = NULL;
    srv->draw_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, qflipper_draw_callback, srv);
    view_port_input_callback_set(view_port, qflipper_input_callback, srv);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    view_port_update(view_port);

#if QFLIPPER_HAVE_COMPOSITE
    srv->rpc = furi_record_open(RECORD_RPC);

    /* Install the composite as a real Flipper Zero (0483:5740). This switches
     * the internal USB PHY to OTG; the USB-Serial-JTAG bridge goes away until
     * the next reboot. */
    if(furi_hal_usb_composite_install(0, 0, NULL, NULL)) {
        /* set_callbacks fires state/ctrl_line immediately with the current
         * line state, so if DTR is already high this seeds a Connected event. */
        furi_hal_cdc_set_callbacks(USB_RPC_CDC_ITF, &usb_rpc_cdc_callbacks, srv);

        qflipper_set_state(srv, QflipperStateActive);
        view_port_update(view_port);

        qflipper_run(srv);

        /* Tear the bridge down before freeing srv: the TinyUSB callbacks hold
         * a pointer to it. */
        furi_hal_cdc_set_callbacks(USB_RPC_CDC_ITF, NULL, NULL);
        usb_rpc_session_close(srv);

        /* Switch USB back to USB-Serial-JTAG so the board is flashable again
         * without a reboot. Experimental — see furi_hal_usb_composite_uninstall. */
        furi_hal_usb_composite_uninstall();
    } else {
        FURI_LOG_E(TAG, "composite_install failed");
        srv->error_msg = "USB composite\ninstall failed";
        qflipper_set_state(srv, QflipperStateError);
        view_port_update(view_port);
        qflipper_run(srv);
    }

    furi_record_close(RECORD_RPC);
#else
    srv->error_msg = "qFlipper needs\nUSB-OTG\n(ESP32-S3 only)";
    qflipper_set_state(srv, QflipperStateError);
    view_port_update(view_port);
    qflipper_run(srv);
#endif

    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(view_port);
    furi_message_queue_free(srv->event_q);
    furi_mutex_free(srv->draw_mutex);
    free(srv);

    return 0;
}
