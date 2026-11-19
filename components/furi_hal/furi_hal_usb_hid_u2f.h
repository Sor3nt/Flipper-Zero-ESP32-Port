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
/** Configured by the host. Stays true while the host keeps the bus suspended. */
bool furi_hal_usb_u2f_backend_is_mounted(void);
void furi_hal_usb_u2f_backend_set_mounted(bool mounted);
/** SET_CONFIGURATION (TinyUSB task): always reported as a fresh Connected, even
 * if the key already looked mounted, because it starts a new host session. */
void furi_hal_usb_u2f_backend_set_configured(void);
/** USB bus suspend (TinyUSB task). Not a disconnect: without VBUS sensing an idle
 * host and a pulled cable look the same, so the key stays mounted. */
void furi_hal_usb_u2f_backend_suspend(void);
/** Drop the mount if, and only if, the bus is still suspended. For callers that
 * learned from another source (VBUS sense) that the cable is gone. */
bool furi_hal_usb_u2f_backend_detach_if_suspended(void);
const uint8_t* furi_hal_usb_u2f_report_descriptor(void);

void furi_hal_hid_u2f_set_callback(HidU2fCallback callback, void* context);
uint32_t furi_hal_hid_u2f_get_request(uint8_t* buffer);
bool furi_hal_hid_u2f_send_response(const uint8_t* buffer, uint32_t length);
void furi_hal_hid_u2f_receive_report(const uint8_t* buffer, uint16_t length);

#ifdef __cplusplus
}
#endif
