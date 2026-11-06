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
            furi_delay_ms(1);
            if(!srv->session_open || qflipper_bridge_is_exiting(srv)) return;
            continue;
        }

        size_t to_read = avail < sizeof(srv->rx_buf) ? avail : sizeof(srv->rx_buf);
        int32_t got = furi_hal_cdc_receive(USB_RPC_CDC_ITF, srv->rx_buf, to_read);
        if(got <= 0) {
            break;
        }

        size_t fed = usb_rpc_feed(srv->session, srv->rx_buf, (size_t)got, srv);
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
        /* While the host is gone after a session, wait with a timeout: if nothing
         * happens within the grace period, ask the desktop to leave qFlipper mode. */
        uint32_t wait = srv->auto_off_armed ? furi_ms_to_ticks(QFLIPPER_BRIDGE_AUTO_OFF_MS) :
                                              FuriWaitForever;
        FuriStatus st = furi_message_queue_get(srv->event_q, &ev, wait);
        if(st == FuriStatusErrorTimeout) {
            if(srv->auto_off_armed && !srv->connected && !qflipper_bridge_is_exiting(srv)) {
                srv->auto_off_armed = false;
                FURI_LOG_I(TAG, "host gone for %u ms — leaving qFlipper mode", (unsigned)QFLIPPER_BRIDGE_AUTO_OFF_MS);
                if(s_auto_off_cb) s_auto_off_cb(s_auto_off_ctx);
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
            qflipper_cli_send_prompt();
            break;

        case UsbRpcEventDisconnected:
            if(!srv->connected) break;
            srv->connected = false;
            srv->rpc_mode = false;
            srv->cli_len = 0;
            srv->auto_off_armed = true;
            FURI_LOG_I(TAG, "DTR down — closing RPC session");
            usb_rpc_session_close(srv);
            break;

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
