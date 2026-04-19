#include "furi_hal_usb_cdc.h"

#include <string.h>

#include <core/common_defines.h>

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
        interface->callbacks->state_callback(interface->context, CdcStateDisconnected);
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

void furi_hal_cdc_send(uint8_t if_num, uint8_t* buf, uint16_t len) {
    UNUSED(buf);
    UNUSED(len);

    FuriHalUsbCdcInterfaceState* interface = furi_hal_usb_cdc_get(if_num);
    if(interface && interface->callbacks && interface->callbacks->tx_ep_callback) {
        interface->callbacks->tx_ep_callback(interface->context);
    }
}

int32_t furi_hal_cdc_receive(uint8_t if_num, uint8_t* buf, uint16_t max_len) {
    FuriHalUsbCdcInterfaceState* interface = furi_hal_usb_cdc_get(if_num);
    if(!interface || !buf || !max_len) {
        return 0;
    }

    memset(buf, 0, max_len);
    return 0;
}
