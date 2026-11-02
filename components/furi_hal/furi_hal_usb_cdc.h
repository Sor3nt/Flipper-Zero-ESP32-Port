#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define CDC_DATA_SZ 64

#ifdef __cplusplus
extern "C" {
#endif

struct usb_cdc_line_coding {
    uint32_t dwDTERate;
    uint8_t bCharFormat;
    uint8_t bParityType;
    uint8_t bDataBits;
};

typedef enum {
    CdcStateDisconnected,
    CdcStateConnected,
} CdcState;

typedef enum {
    CdcCtrlLineDTR = (1 << 0),
    CdcCtrlLineRTS = (1 << 1),
} CdcCtrlLine;

typedef struct {
    void (*tx_ep_callback)(void* context);
    void (*rx_ep_callback)(void* context);
    void (*state_callback)(void* context, CdcState state);
    void (*ctrl_line_callback)(void* context, CdcCtrlLine ctrl_lines);
    void (*config_callback)(void* context, struct usb_cdc_line_coding* config);
} CdcCallbacks;

void furi_hal_cdc_set_callbacks(uint8_t if_num, CdcCallbacks* cb, void* context);

/** true while some owner (CLI shell, qFlipper bridge, an app) has callbacks on this CDC.
 *  Callbacks are exclusive: set them only on an interface that is not owned. */
bool furi_hal_cdc_is_owned(uint8_t if_num);
bool furi_hal_cdc_get_port_settings(uint8_t if_num, struct usb_cdc_line_coding* settings);
uint8_t furi_hal_cdc_get_ctrl_line_state(uint8_t if_num);
void furi_hal_cdc_send(uint8_t if_num, uint8_t* buf, uint16_t len);
int32_t furi_hal_cdc_send_nonblocking(uint8_t if_num, const uint8_t* buf, uint16_t len);
int32_t furi_hal_cdc_receive(uint8_t if_num, uint8_t* buf, uint16_t max_len);

#ifdef __cplusplus
}
#endif
