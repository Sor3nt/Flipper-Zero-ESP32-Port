#include "furi_hal_usb.h"
#include "furi_hal_usb_hid_backend.h"

#include <stddef.h>

static FuriHalUsbInterface* furi_hal_usb_current = NULL;
static FuriHalUsbStateCallback furi_hal_usb_state_callback = NULL;
static void* furi_hal_usb_state_context = NULL;
static bool furi_hal_usb_locked = false;

FuriHalUsbInterface usb_cdc_single = {.name = "cdc_single"};
FuriHalUsbInterface usb_cdc_dual = {.name = "cdc_dual"};
FuriHalUsbInterface usb_hid = {.name = "hid"};
FuriHalUsbInterface usb_hid_u2f = {.name = "hid_u2f"};
FuriHalUsbInterface usb_ccid = {.name = "ccid"};

void furi_hal_usb_init(void) {
    furi_hal_usb_current = NULL;
    furi_hal_usb_locked = false;
}

bool furi_hal_usb_set_config(FuriHalUsbInterface* new_if, void* ctx) {
    if(furi_hal_usb_locked) {
        return false;
    }

    FuriHalUsbInterface* prev = furi_hal_usb_current;
    furi_hal_usb_current = new_if;

    if(prev == &usb_hid && new_if != &usb_hid) {
        furi_hal_usb_hid_backend_stop();
    }

    if(new_if == &usb_hid) {
        if(!furi_hal_usb_hid_backend_start((const FuriHalUsbHidConfig*)ctx)) {
            furi_hal_usb_current = prev;
            return false;
        }
    }

    if(furi_hal_usb_state_callback) {
        furi_hal_usb_state_callback(
            FuriHalUsbStateEventDescriptorRequest, furi_hal_usb_state_context);
    }

    return true;
}

FuriHalUsbInterface* furi_hal_usb_get_config(void) {
    return furi_hal_usb_current;
}

void furi_hal_usb_lock(void) {
    furi_hal_usb_locked = true;
}

void furi_hal_usb_unlock(void) {
    furi_hal_usb_locked = false;
}

bool furi_hal_usb_is_locked(void) {
    return furi_hal_usb_locked;
}

void furi_hal_usb_disable(void) {
    if(furi_hal_usb_current == &usb_hid) {
        furi_hal_usb_hid_backend_stop();
    }
}

void furi_hal_usb_enable(void) {
}

void furi_hal_usb_set_state_callback(FuriHalUsbStateCallback cb, void* ctx) {
    furi_hal_usb_state_callback = cb;
    furi_hal_usb_state_context = ctx;
}

void furi_hal_usb_reinit(void) {
}
