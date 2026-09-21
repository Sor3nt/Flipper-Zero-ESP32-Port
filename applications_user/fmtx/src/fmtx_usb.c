#include "fmtx_usb.h"

/* Stub: this port's USB stack (TinyUSB-based FuriHalUsbInterface, see
 * furi_hal_usb.h) does not expose the raw device/descriptor/endpoint API
 * (usbd_device, usbd_ep_config, usb_std.h descriptors, ...) that upstream's
 * USB-audio-speaker source relies on -- that API is specific to the real
 * Flipper Zero's libusb_stm32-based USB HAL. Implementing an equivalent
 * TinyUSB UAC (USB Audio Class) driver is out of scope for this port, so the
 * USB source is disabled here (see fmtx_scenes.c: the "Source" setting only
 * offers "MP3"); these stubs just keep the rest of fmtx_playback.c linking
 * against the same interface without ever being reachable from the UI. */

bool fmtx_usb_start(FmtxUsbRx callback, void* ctx) {
    (void)callback;
    (void)ctx;
    return false;
}

bool fmtx_usb_stop(void) {
    return true;
}

bool fmtx_usb_connected(void) {
    return false;
}
