#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HID_U2F_PACKET_LEN 64

typedef enum {
    HidU2fConnected,
    HidU2fDisconnected,
    HidU2fRequest,
} HidU2fEvent;

typedef void (*HidU2fCallback)(HidU2fEvent event, void* context);

bool furi_hal_usb_u2f_backend_start(void);
bool furi_hal_usb_u2f_backend_stop(void);
bool furi_hal_usb_u2f_backend_is_active(void);
bool furi_hal_usb_u2f_backend_is_mounted(void);
void furi_hal_usb_u2f_backend_set_mounted(bool mounted);
const uint8_t* furi_hal_usb_u2f_report_descriptor(void);

void furi_hal_hid_u2f_set_callback(HidU2fCallback callback, void* context);
uint32_t furi_hal_hid_u2f_get_request(uint8_t* buffer);
bool furi_hal_hid_u2f_send_response(const uint8_t* buffer, uint32_t length);
void furi_hal_hid_u2f_receive_report(const uint8_t* buffer, uint16_t length);

#ifdef __cplusplus
}
#endif
