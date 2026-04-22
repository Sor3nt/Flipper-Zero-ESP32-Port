#pragma once

#include "furi_hal_usb_hid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Internal HID backend interface.
 * Two implementations ship: a TinyUSB-based one for SoCs with USB-OTG
 * (furi_hal_usb_hid_tinyusb.c) and a no-op stub for others
 * (furi_hal_usb_hid_stub.c). furi_hal_usb.c dispatches here when the
 * usb_hid interface is selected via furi_hal_usb_set_config(). */
bool furi_hal_usb_hid_backend_start(const FuriHalUsbHidConfig* cfg);
void furi_hal_usb_hid_backend_stop(void);

#ifdef __cplusplus
}
#endif
