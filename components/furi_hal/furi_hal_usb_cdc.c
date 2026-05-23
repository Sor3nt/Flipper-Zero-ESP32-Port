#include "furi_hal_usb_cdc.h"

#include <string.h>

#include <core/common_defines.h>
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2
#include <furi.h>
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
    struct usb_cdc_line_coding line_coding;
    uint8_t ctrl_line_state;
} FuriHalUsbCdcInterfaceState;

static FuriHalUsbCdcInterfaceState furi_hal_usb_cdc[FURI_HAL_USB_CDC_IF_MAX] = {
    [0 ... (FURI_HAL_USB_CDC_IF_MAX - 1)] =
        {
            .callbacks = NULL,
            .context = NULL,
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

void furi_hal_cdc_set_callbacks(uint8_t if_num, CdcCallbacks* cb, void* context) {
    FuriHalUsbCdcInterfaceState* interface = furi_hal_usb_cdc_get(if_num);
    if(!interface) {
        return;
    }

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
}

struct usb_cdc_line_coding* furi_hal_cdc_get_port_settings(uint8_t if_num) {
    FuriHalUsbCdcInterfaceState* interface = furi_hal_usb_cdc_get(if_num);
    return interface ? &interface->line_coding : NULL;
}

uint8_t furi_hal_cdc_get_ctrl_line_state(uint8_t if_num) {
    FuriHalUsbCdcInterfaceState* interface = furi_hal_usb_cdc_get(if_num);
    return interface ? interface->ctrl_line_state : 0;
}

#if FURI_HAL_CDC_HAVE_TINYUSB

#define TAG "FuriHalCdc"

/* The composite descriptor exposes a single CDC interface, wired to esp_tinyusb
 * logical port TINYUSB_CDC_ACM_0. furi if_num 0 maps to it 1:1. */

void furi_hal_cdc_send(uint8_t if_num, uint8_t* buf, uint16_t len) {
    FuriHalUsbCdcInterfaceState* interface = furi_hal_usb_cdc_get(if_num);
    if(!interface || if_num >= TINYUSB_CDC_ACM_MAX) {
        return;
    }

    /* write_queue copies into the 512-byte TX ring; flush kicks the USB
     * transfer and frees ring space. For payloads larger than the ring (RPC
     * screen frames, file reads) we loop, flushing between chunks. A bounded
     * retry guards against a host that stops reading mid-transfer. */
    size_t sent = 0;
    int stalls = 0;
    while(sent < len) {
        size_t queued =
            tinyusb_cdcacm_write_queue(if_num, buf + sent, (size_t)(len - sent));
        sent += queued;
        tinyusb_cdcacm_write_flush(if_num, pdMS_TO_TICKS(50));

        if(queued == 0) {
            if(++stalls > 200) { /* ~2 s of no progress: give up */
                FURI_LOG_W(TAG, "cdc tx stalled, dropping %u bytes", (unsigned)(len - sent));
                break;
            }
            furi_delay_ms(10);
        } else {
            stalls = 0;
        }
    }

    if(interface->callbacks && interface->callbacks->tx_ep_callback) {
        interface->callbacks->tx_ep_callback(interface->context);
    }
}

int32_t furi_hal_cdc_receive(uint8_t if_num, uint8_t* buf, uint16_t max_len) {
    FuriHalUsbCdcInterfaceState* interface = furi_hal_usb_cdc_get(if_num);
    if(!interface || !buf || !max_len || if_num >= TINYUSB_CDC_ACM_MAX) {
        return 0;
    }

    size_t rx_size = 0;
    esp_err_t err = tinyusb_cdcacm_read(if_num, buf, max_len, &rx_size);
    if(err != ESP_OK) {
        return 0;
    }
    return (int32_t)rx_size;
}

/* ─────────────────────────────────────────────────────────────────────
 * TinyUSB CDC-ACM event fan-out → user CdcCallbacks.
 * These run on the TinyUSB task.
 * ───────────────────────────────────────────────────────────────────── */

static void furi_hal_cdc_tusb_rx(int itf, cdcacm_event_t* event) {
    UNUSED(event);
    FuriHalUsbCdcInterfaceState* interface = furi_hal_usb_cdc_get((uint8_t)itf);
    if(interface && interface->callbacks && interface->callbacks->rx_ep_callback) {
        interface->callbacks->rx_ep_callback(interface->context);
    }
}

static void furi_hal_cdc_tusb_line_state(int itf, cdcacm_event_t* event) {
    FuriHalUsbCdcInterfaceState* interface = furi_hal_usb_cdc_get((uint8_t)itf);
    if(!interface) {
        return;
    }

    uint8_t ctrl = 0;
    if(event->line_state_changed_data.dtr) ctrl |= CdcCtrlLineDTR;
    if(event->line_state_changed_data.rts) ctrl |= CdcCtrlLineRTS;
    interface->ctrl_line_state = ctrl;

    if(interface->callbacks && interface->callbacks->ctrl_line_callback) {
        interface->callbacks->ctrl_line_callback(interface->context, ctrl);
    }
}

static void furi_hal_cdc_tusb_line_coding(int itf, cdcacm_event_t* event) {
    FuriHalUsbCdcInterfaceState* interface = furi_hal_usb_cdc_get((uint8_t)itf);
    if(!interface) {
        return;
    }

    const cdc_line_coding_t* lc = event->line_coding_changed_data.p_line_coding;
    if(lc) {
        interface->line_coding.dwDTERate = lc->bit_rate;
        interface->line_coding.bCharFormat = lc->stop_bits;
        interface->line_coding.bParityType = lc->parity;
        interface->line_coding.bDataBits = lc->data_bits;
    }

    if(interface->callbacks && interface->callbacks->config_callback) {
        interface->callbacks->config_callback(interface->context, &interface->line_coding);
    }
}

/* Wire the tinyusb CDC events into the fan-out above. Called once by the
 * composite installer right after tusb_cdc_acm_init (which registers the
 * interface with NULL callbacks). */
void furi_hal_cdc_internal_register_tinyusb_callbacks(void) {
    tinyusb_cdcacm_register_callback(TINYUSB_CDC_ACM_0, CDC_EVENT_RX, furi_hal_cdc_tusb_rx);
    tinyusb_cdcacm_register_callback(
        TINYUSB_CDC_ACM_0, CDC_EVENT_LINE_STATE_CHANGED, furi_hal_cdc_tusb_line_state);
    tinyusb_cdcacm_register_callback(
        TINYUSB_CDC_ACM_0, CDC_EVENT_LINE_CODING_CHANGED, furi_hal_cdc_tusb_line_coding);
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

#endif /* FURI_HAL_CDC_HAVE_TINYUSB */
