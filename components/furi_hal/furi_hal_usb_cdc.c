#include "furi_hal_usb_cdc.h"

#include <string.h>

#include <core/common_defines.h>
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2
#include <furi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#define FURI_HAL_CDC_HAVE_TINYUSB 1
#else
#define FURI_HAL_CDC_HAVE_TINYUSB 0
#endif

#define FURI_HAL_USB_CDC_IF_MAX 2

typedef struct {
    CdcCallbacks* callbacks;
    void* context;
    uint32_t callbacks_in_flight;
    struct usb_cdc_line_coding line_coding;
    uint8_t ctrl_line_state;
} FuriHalUsbCdcInterfaceState;

static FuriHalUsbCdcInterfaceState furi_hal_usb_cdc[FURI_HAL_USB_CDC_IF_MAX] = {
    [0 ... (FURI_HAL_USB_CDC_IF_MAX - 1)] =
        {
            .callbacks = NULL,
            .context = NULL,
            .callbacks_in_flight = 0,
            .line_coding =
                {
                    .dwDTERate = 115200,
                    .bCharFormat = 0,
                    .bParityType = 0,
                    .bDataBits = 8,
                },
            .ctrl_line_state = 0,
        },
};

static FuriHalUsbCdcInterfaceState* furi_hal_usb_cdc_get(uint8_t if_num) {
    if(if_num >= FURI_HAL_USB_CDC_IF_MAX) {
        return NULL;
    }

    return &furi_hal_usb_cdc[if_num];
}

#if FURI_HAL_CDC_HAVE_TINYUSB
static portMUX_TYPE furi_hal_usb_cdc_callback_lock = portMUX_INITIALIZER_UNLOCKED;

static CdcCallbacks* furi_hal_usb_cdc_callback_acquire(
    FuriHalUsbCdcInterfaceState* interface,
    void** context) {
    portENTER_CRITICAL(&furi_hal_usb_cdc_callback_lock);
    CdcCallbacks* callbacks = interface->callbacks;
    *context = interface->context;
    if(callbacks) interface->callbacks_in_flight++;
    portEXIT_CRITICAL(&furi_hal_usb_cdc_callback_lock);
    return callbacks;
}

static void furi_hal_usb_cdc_callback_release(FuriHalUsbCdcInterfaceState* interface) {
    portENTER_CRITICAL(&furi_hal_usb_cdc_callback_lock);
    furi_assert(interface->callbacks_in_flight > 0);
    interface->callbacks_in_flight--;
    portEXIT_CRITICAL(&furi_hal_usb_cdc_callback_lock);
}

static void furi_hal_usb_cdc_callbacks_wait_idle(FuriHalUsbCdcInterfaceState* interface) {
    while(true) {
        portENTER_CRITICAL(&furi_hal_usb_cdc_callback_lock);
        bool idle = interface->callbacks_in_flight == 0;
        portEXIT_CRITICAL(&furi_hal_usb_cdc_callback_lock);
        if(idle) break;
        furi_delay_ms(1);
    }
}
#endif

void furi_hal_cdc_set_callbacks(uint8_t if_num, CdcCallbacks* cb, void* context) {
    FuriHalUsbCdcInterfaceState* interface = furi_hal_usb_cdc_get(if_num);
    if(!interface) {
        return;
    }

#if FURI_HAL_CDC_HAVE_TINYUSB
    portENTER_CRITICAL(&furi_hal_usb_cdc_callback_lock);
    interface->callbacks = cb;
    interface->context = context;
    struct usb_cdc_line_coding line_coding = interface->line_coding;
    uint8_t ctrl_line_state = interface->ctrl_line_state;
    if(cb) interface->callbacks_in_flight++;
    portEXIT_CRITICAL(&furi_hal_usb_cdc_callback_lock);

    if(cb) {
        if(cb->state_callback) {
            cb->state_callback(
                context,
                (ctrl_line_state & CdcCtrlLineDTR) ? CdcStateConnected :
                                                     CdcStateDisconnected);
        }
        if(cb->ctrl_line_callback) {
            cb->ctrl_line_callback(context, ctrl_line_state);
        }
        if(cb->config_callback) cb->config_callback(context, &line_coding);
        furi_hal_usb_cdc_callback_release(interface);
    } else {
        furi_hal_usb_cdc_callbacks_wait_idle(interface);
    }
#else
    interface->callbacks = cb;
    interface->context = context;
    if(interface->callbacks && interface->callbacks->state_callback) {
        interface->callbacks->state_callback(
            interface->context,
            (interface->ctrl_line_state & CdcCtrlLineDTR) ? CdcStateConnected :
                                                            CdcStateDisconnected);
    }
    if(interface->callbacks && interface->callbacks->ctrl_line_callback) {
        interface->callbacks->ctrl_line_callback(interface->context, interface->ctrl_line_state);
    }
    if(interface->callbacks && interface->callbacks->config_callback) {
        interface->callbacks->config_callback(interface->context, &interface->line_coding);
    }
#endif
}

bool furi_hal_cdc_get_port_settings(uint8_t if_num, struct usb_cdc_line_coding* settings) {
    FuriHalUsbCdcInterfaceState* interface = furi_hal_usb_cdc_get(if_num);
    if(!interface || !settings) return false;
#if FURI_HAL_CDC_HAVE_TINYUSB
    portENTER_CRITICAL(&furi_hal_usb_cdc_callback_lock);
    *settings = interface->line_coding;
    portEXIT_CRITICAL(&furi_hal_usb_cdc_callback_lock);
#else
    *settings = interface->line_coding;
#endif
    return true;
}

uint8_t furi_hal_cdc_get_ctrl_line_state(uint8_t if_num) {
    FuriHalUsbCdcInterfaceState* interface = furi_hal_usb_cdc_get(if_num);
    if(!interface) return 0;
#if FURI_HAL_CDC_HAVE_TINYUSB
    portENTER_CRITICAL(&furi_hal_usb_cdc_callback_lock);
    uint8_t ctrl_line_state = interface->ctrl_line_state;
    portEXIT_CRITICAL(&furi_hal_usb_cdc_callback_lock);
    return ctrl_line_state;
#else
    return interface->ctrl_line_state;
#endif
}

#if FURI_HAL_CDC_HAVE_TINYUSB

#define TAG "FuriHalCdc"

#include <esp_log.h>
#include <esp_attr.h>
#include <furi_hal_qfdiag.h>
#include "device/usbd_pvt.h" /* usbd_defer_func: run a function on the TinyUSB task */

/* ─────────────────────────────────────────────────────────────────────
 * FIX [QF-2]: qFlipper uploads of large files (asset packs, multi-MB) failed
 *   mid-transfer with "Operation timeout"; small files usually passed.
 * ROOT CAUSE: TinyUSB CDC OUT race — packet k lost, packet k+1 delivered
 *   twice (details below). Proven on hardware with the QFDIAG byte trace.
 * WARNING: the TinyUSB task must stay the ONLY caller of tud_cdc_n_read().
 *   Never read the TinyUSB FIFO directly from another task (bridge, CLI, apps):
 *   use furi_hal_cdc_receive(), which reads the staging ring below.
 *
 * In DMA mode (CONFIG_TINYUSB_MODE_DMA → CFG_TUD_EDPT_DEDICATED_HWFIFO=0)
 * TinyUSB receives every CDC OUT packet into one shared epout buffer and
 * copies it into its RX FIFO afterwards, on the TinyUSB task, in
 * cdcd_xfer_cb(). But usbd.c clears the endpoint's BUSY|CLAIMED bits
 * *before* calling cdcd_xfer_cb(), and tud_cdc_n_read() re-arms the
 * endpoint (tu_edpt_stream_read_xfer). A tud_cdc_n_read() from another
 * task that lands in that gap re-arms the endpoint into the same epout
 * buffer, the host's next packet overwrites the one not copied yet, and
 * the stream gets packet k+1 twice instead of k, k+1. The byte count is
 * unchanged, so the data silently desyncs: qFlipper uploads lost their
 * protobuf framing mid-file ("Operation timeout"). The TinyUSB task runs at
 * priority 5 and furi threads (16+) preempt it, which can hold that gap
 * open for milliseconds.
 *
 * So the TinyUSB task moves data from the TinyUSB FIFO into this ring (on
 * every RX event and on request), and furi_hal_cdc_receive() only ever
 * reads the ring. A full ring leaves the rest in the TinyUSB FIFO; the
 * endpoint is then not re-armed and the host is NAKed, the same
 * back-pressure as before. A reader that frees space in a full ring asks
 * the TinyUSB task (usbd_defer_func) to pull again.
 * ───────────────────────────────────────────────────────────────────── */

#define FURI_HAL_CDC_RX_RING_SIZE  2048u /* power of two */
#define FURI_HAL_CDC_RX_RING_MASK  (FURI_HAL_CDC_RX_RING_SIZE - 1u)
#define FURI_HAL_CDC_RX_COPY_CHUNK 128u /* bytes copied per critical section */

typedef struct {
    uint32_t head; /* bytes ever written, by the TinyUSB task */
    uint32_t tail; /* bytes ever read, by readers */
    bool throttled; /* ring filled while the TinyUSB FIFO still held data */
    bool pull_queued; /* a deferred pull is waiting in the TinyUSB queue */
} FuriHalCdcRxRing;

EXT_RAM_BSS_ATTR static uint8_t
    furi_hal_cdc_rx_data[FURI_HAL_USB_CDC_IF_MAX][FURI_HAL_CDC_RX_RING_SIZE];
static FuriHalCdcRxRing furi_hal_cdc_rx_ring[FURI_HAL_USB_CDC_IF_MAX];
static portMUX_TYPE furi_hal_cdc_rx_lock = portMUX_INITIALIZER_UNLOCKED;
/* Learned on the first TinyUSB-task callback; reset on every (re)install. */
static TaskHandle_t volatile furi_hal_cdc_tusb_task = NULL;

static uint32_t furi_hal_cdc_rx_used(uint8_t itf) {
    portENTER_CRITICAL(&furi_hal_cdc_rx_lock);
    const uint32_t used = furi_hal_cdc_rx_ring[itf].head - furi_hal_cdc_rx_ring[itf].tail;
    portEXIT_CRITICAL(&furi_hal_cdc_rx_lock);
    return used;
}

/* FIX [QF-2]: TinyUSB task only: move TinyUSB FIFO → ring. Never calls user
 * callbacks. WARNING: calling this (or tud_cdc_n_read) from any other task
 * reintroduces the epout_buf race. */
static uint32_t furi_hal_cdc_rx_pull(uint8_t itf) {
    furi_hal_cdc_tusb_task = xTaskGetCurrentTaskHandle();
    FuriHalCdcRxRing* ring = &furi_hal_cdc_rx_ring[itf];
    uint8_t* data = furi_hal_cdc_rx_data[itf];
    uint8_t chunk[FURI_HAL_CDC_RX_COPY_CHUNK];
    uint32_t moved = 0;

    while(true) {
        const uint32_t avail = tud_cdc_n_available(itf);
        if(avail == 0) break;

        /* Space check and throttle flag under the readers' lock: a reader
         * either frees space before this check (and we keep pulling) or
         * finds the flag set afterwards (and requests another pull). */
        portENTER_CRITICAL(&furi_hal_cdc_rx_lock);
        const uint32_t space = FURI_HAL_CDC_RX_RING_SIZE - (ring->head - ring->tail);
        ring->throttled = (space == 0);
        portEXIT_CRITICAL(&furi_hal_cdc_rx_lock);
        if(space == 0) break;

        uint32_t want = avail < space ? avail : space;
        if(want > sizeof(chunk)) want = sizeof(chunk);
        const uint32_t got = tud_cdc_n_read(itf, chunk, want);
        if(got == 0) break;

        portENTER_CRITICAL(&furi_hal_cdc_rx_lock);
        const uint32_t pos = ring->head & FURI_HAL_CDC_RX_RING_MASK;
        const uint32_t first =
            got < (FURI_HAL_CDC_RX_RING_SIZE - pos) ? got : (FURI_HAL_CDC_RX_RING_SIZE - pos);
        memcpy(&data[pos], chunk, first);
        memcpy(data, chunk + first, got - first);
        ring->head += got;
        portEXIT_CRITICAL(&furi_hal_cdc_rx_lock);
        moved += got;
    }
    return moved;
}

static void furi_hal_cdc_rx_notify(uint8_t itf) {
    FuriHalUsbCdcInterfaceState* interface = furi_hal_usb_cdc_get(itf);
    if(!interface) return;
    void* context = NULL;
    CdcCallbacks* callbacks = furi_hal_usb_cdc_callback_acquire(interface, &context);
    if(callbacks) {
        if(callbacks->rx_ep_callback) callbacks->rx_ep_callback(context);
        furi_hal_usb_cdc_callback_release(interface);
    }
}

/* TinyUSB task: pull, then tell the reader there is data. */
static void furi_hal_cdc_rx_service(uint8_t itf) {
    furi_hal_cdc_rx_pull(itf);
    if(furi_hal_cdc_rx_used(itf) > 0) furi_hal_cdc_rx_notify(itf);
}

static void furi_hal_cdc_rx_deferred_pull(void* param) {
    const uint8_t itf = (uint8_t)(uintptr_t)param;
    portENTER_CRITICAL(&furi_hal_cdc_rx_lock);
    furi_hal_cdc_rx_ring[itf].pull_queued = false;
    portEXIT_CRITICAL(&furi_hal_cdc_rx_lock);
    furi_hal_cdc_rx_service(itf);
}

static void furi_hal_cdc_rx_request_pull(uint8_t itf) {
    if(xTaskGetCurrentTaskHandle() == furi_hal_cdc_tusb_task) {
        /* Reader running inside a TinyUSB callback: pulling here is safe and
         * pull() calls no callbacks, so this cannot recurse. */
        furi_hal_cdc_rx_pull(itf);
        return;
    }
    if(!tud_inited()) return;

    bool queue = false;
    portENTER_CRITICAL(&furi_hal_cdc_rx_lock);
    if(!furi_hal_cdc_rx_ring[itf].pull_queued) {
        furi_hal_cdc_rx_ring[itf].pull_queued = true;
        queue = true;
    }
    portEXIT_CRITICAL(&furi_hal_cdc_rx_lock);
    if(queue) usbd_defer_func(furi_hal_cdc_rx_deferred_pull, (void*)(uintptr_t)itf, false);
}

/* QFDIAG */
static volatile uint32_t furi_hal_cdc_tx_since_ms;

uint32_t furi_hal_cdc_rx_available(uint8_t if_num) {
    if(if_num >= TINYUSB_CDC_ACM_MAX || if_num >= FURI_HAL_USB_CDC_IF_MAX) return 0;
    return furi_hal_cdc_rx_used(if_num) + tud_cdc_n_available(if_num);
}

uint32_t furi_hal_cdc_tx_inflight_ms(void) {
    const uint32_t since = furi_hal_cdc_tx_since_ms;
    return since ? furi_get_tick() - since : 0;
}

/* The composite descriptor exposes a single CDC interface, wired to esp_tinyusb
 * logical port TINYUSB_CDC_ACM_0. furi if_num 0 maps to it 1:1. */

void furi_hal_cdc_send(uint8_t if_num, uint8_t* buf, uint16_t len) {
    FuriHalUsbCdcInterfaceState* interface = furi_hal_usb_cdc_get(if_num);
    if(!interface || if_num >= TINYUSB_CDC_ACM_MAX) {
        return;
    }

    /* write_queue copies into the 512-byte TX ring; flush kicks the USB
     * transfer and frees ring space. For payloads larger than the ring (RPC
     * screen frames, file reads) we loop, flushing between chunks.
     *
     * These bytes are framed CLI/RPC output: dropping part of a message desyncs
     * the protobuf stream on the host, which makes qFlipper abort a transfer as
     * corrupt. So we must NOT truncate while the host is still there — a host
     * that is slow to drain its IN endpoint (e.g. busy pushing a file upload)
     * only needs USB back-pressure, not data loss. We keep retrying and bail
     * only once the port is actually gone: tud_cdc_n_connected() is false when
     * the device is unmounted/suspended (no VBUS sense here: unplug == suspend)
     * or DTR has dropped, so a vanished host can never wedge this loop. */
    size_t sent = 0;
    const uint32_t qd_t0 = QFDIAG_ENABLED ? furi_get_tick() : 0; /* QFDIAG */
    if(QFDIAG_ENABLED) furi_hal_cdc_tx_since_ms = qd_t0 ? qd_t0 : 1;
    uint32_t qd_stalls = 0;
    while(sent < len) {
        size_t queued =
            tinyusb_cdcacm_write_queue(if_num, buf + sent, (size_t)(len - sent));
        sent += queued;
        tinyusb_cdcacm_write_flush(if_num, pdMS_TO_TICKS(50));

        if(queued == 0) {
            qd_stalls++;
            if(!tud_cdc_n_connected(if_num)) {
                FURI_LOG_W(
                    TAG, "cdc tx: port closed, dropping %u bytes", (unsigned)(len - sent));
                break;
            }
            furi_delay_ms(10);
        }
    }
    furi_hal_cdc_tx_since_ms = 0;
    const uint32_t qd_td = QFDIAG_ENABLED ? furi_get_tick() - qd_t0 : 0;
    if(QFDIAG_ENABLED && qd_td > 200) {
        ESP_LOGW(
            TAG,
            "QD cdc tx %u/%u bytes took %lu ms (%lu full-FIFO retries)",
            (unsigned)sent,
            (unsigned)len,
            (unsigned long)qd_td,
            (unsigned long)qd_stalls);
    }

    void* context = NULL;
    CdcCallbacks* callbacks = furi_hal_usb_cdc_callback_acquire(interface, &context);
    if(callbacks) {
        if(callbacks->tx_ep_callback) callbacks->tx_ep_callback(context);
        furi_hal_usb_cdc_callback_release(interface);
    }
}

int32_t furi_hal_cdc_receive(uint8_t if_num, uint8_t* buf, uint16_t max_len) {
    FuriHalUsbCdcInterfaceState* interface = furi_hal_usb_cdc_get(if_num);
    if(!interface || !buf || !max_len || if_num >= TINYUSB_CDC_ACM_MAX) {
        return 0;
    }

    /* FIX [QF-2]: reads the staging ring only; never tud_cdc_n_read() here —
     * this runs on the caller's task, and re-arming the OUT endpoint from a
     * non-TinyUSB task is exactly the race that corrupted uploads. */
    FuriHalCdcRxRing* ring = &furi_hal_cdc_rx_ring[if_num];
    const uint8_t* data = furi_hal_cdc_rx_data[if_num];
    uint32_t total = 0;
    bool request_pull = false;

    while(total < max_len) {
        portENTER_CRITICAL(&furi_hal_cdc_rx_lock);
        const uint32_t used = ring->head - ring->tail;
        uint32_t n = used < (max_len - total) ? used : (max_len - total);
        if(n > FURI_HAL_CDC_RX_COPY_CHUNK) n = FURI_HAL_CDC_RX_COPY_CHUNK;
        if(n) {
            const uint32_t pos = ring->tail & FURI_HAL_CDC_RX_RING_MASK;
            const uint32_t first =
                n < (FURI_HAL_CDC_RX_RING_SIZE - pos) ? n : (FURI_HAL_CDC_RX_RING_SIZE - pos);
            memcpy(buf + total, &data[pos], first);
            memcpy(buf + total + first, data, n - first);
            ring->tail += n;
        }
        if(ring->throttled && (ring->head - ring->tail) < FURI_HAL_CDC_RX_RING_SIZE) {
            ring->throttled = false;
            request_pull = true;
        }
        portEXIT_CRITICAL(&furi_hal_cdc_rx_lock);
        if(n == 0) break;
        total += n;
    }

    if(request_pull) furi_hal_cdc_rx_request_pull(if_num);
    return (int32_t)total;
}

/* ─────────────────────────────────────────────────────────────────────
 * TinyUSB CDC-ACM event fan-out → user CdcCallbacks.
 * These run on the TinyUSB task.
 * ───────────────────────────────────────────────────────────────────── */

/* FIX [QF-2]: the RX event only pulls into the ring on the TinyUSB task and
 * then notifies; user callbacks must not call tud_cdc_n_read(). */
static void furi_hal_cdc_tusb_rx(int itf, cdcacm_event_t* event) {
    UNUSED(event);
    if(itf < 0 || itf >= TINYUSB_CDC_ACM_MAX || itf >= FURI_HAL_USB_CDC_IF_MAX) return;
    furi_hal_cdc_rx_service((uint8_t)itf);
}

static void furi_hal_cdc_tusb_line_state(int itf, cdcacm_event_t* event) {
    FuriHalUsbCdcInterfaceState* interface = furi_hal_usb_cdc_get((uint8_t)itf);
    if(!interface) {
        return;
    }

    uint8_t ctrl = 0;
    if(event->line_state_changed_data.dtr) ctrl |= CdcCtrlLineDTR;
    if(event->line_state_changed_data.rts) ctrl |= CdcCtrlLineRTS;
    portENTER_CRITICAL(&furi_hal_usb_cdc_callback_lock);
    interface->ctrl_line_state = ctrl;
    portEXIT_CRITICAL(&furi_hal_usb_cdc_callback_lock);
    void* context = NULL;
    CdcCallbacks* callbacks = furi_hal_usb_cdc_callback_acquire(interface, &context);
    if(callbacks) {
        if(callbacks->ctrl_line_callback) callbacks->ctrl_line_callback(context, ctrl);
        furi_hal_usb_cdc_callback_release(interface);
    }
}

static void furi_hal_cdc_tusb_line_coding(int itf, cdcacm_event_t* event) {
    FuriHalUsbCdcInterfaceState* interface = furi_hal_usb_cdc_get((uint8_t)itf);
    if(!interface) {
        return;
    }

    const cdc_line_coding_t* lc = event->line_coding_changed_data.p_line_coding;
    struct usb_cdc_line_coding line_coding;
    portENTER_CRITICAL(&furi_hal_usb_cdc_callback_lock);
    if(lc) {
        interface->line_coding.dwDTERate = lc->bit_rate;
        interface->line_coding.bCharFormat = lc->stop_bits;
        interface->line_coding.bParityType = lc->parity;
        interface->line_coding.bDataBits = lc->data_bits;
    }
    line_coding = interface->line_coding;
    portEXIT_CRITICAL(&furi_hal_usb_cdc_callback_lock);

    void* context = NULL;
    CdcCallbacks* callbacks = furi_hal_usb_cdc_callback_acquire(interface, &context);
    if(callbacks) {
        if(callbacks->config_callback) callbacks->config_callback(context, &line_coding);
        furi_hal_usb_cdc_callback_release(interface);
    }
}

/* Wire the tinyusb CDC events into the fan-out above. Called once by the
 * composite installer right after tusb_cdc_acm_init (which registers the
 * interface with NULL callbacks). */
void furi_hal_cdc_internal_register_tinyusb_callbacks(void) {
    /* Fresh CDC instance: TinyUSB's FIFO was just reset, drop our stale bytes
     * too, and forget the old TinyUSB task (it may have been recreated). */
    portENTER_CRITICAL(&furi_hal_cdc_rx_lock);
    memset(furi_hal_cdc_rx_ring, 0, sizeof(furi_hal_cdc_rx_ring));
    furi_hal_cdc_tusb_task = NULL;
    portEXIT_CRITICAL(&furi_hal_cdc_rx_lock);

    tinyusb_cdcacm_register_callback(TINYUSB_CDC_ACM_0, CDC_EVENT_RX, furi_hal_cdc_tusb_rx);
    tinyusb_cdcacm_register_callback(
        TINYUSB_CDC_ACM_0, CDC_EVENT_LINE_STATE_CHANGED, furi_hal_cdc_tusb_line_state);
    tinyusb_cdcacm_register_callback(
        TINYUSB_CDC_ACM_0, CDC_EVENT_LINE_CODING_CHANGED, furi_hal_cdc_tusb_line_coding);

    /* Anything that reached the TinyUSB FIFO before the RX callback was wired
     * would otherwise wait for the next packet; have the TinyUSB task pull now. */
    furi_hal_cdc_rx_request_pull(TINYUSB_CDC_ACM_0);
}

#else /* !FURI_HAL_CDC_HAVE_TINYUSB — boards without USB-OTG */

void furi_hal_cdc_send(uint8_t if_num, uint8_t* buf, uint16_t len) {
    UNUSED(if_num);
    UNUSED(buf);
    UNUSED(len);
}

int32_t furi_hal_cdc_receive(uint8_t if_num, uint8_t* buf, uint16_t max_len) {
    UNUSED(if_num);
    if(buf && max_len) {
        memset(buf, 0, max_len);
    }
    return 0;
}

void furi_hal_cdc_internal_register_tinyusb_callbacks(void) {
}

uint32_t furi_hal_cdc_rx_available(uint8_t if_num) {
    UNUSED(if_num);
    return 0;
}

uint32_t furi_hal_cdc_tx_inflight_ms(void) {
    return 0;
}

#endif /* FURI_HAL_CDC_HAVE_TINYUSB */
