#include "furi_hal_usb.h"
#include "furi_hal_usb_hid_backend.h"
#include "furi_hal_usb_tinyusb_composite.h"

#include <furi.h>
#include <stddef.h>
#include <string.h>

#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "furi_hal_usb_hid_u2f.h"
#endif

static FuriHalUsbInterface* furi_hal_usb_current = NULL;
static FuriHalUsbStateCallback furi_hal_usb_state_callback = NULL;
static void* furi_hal_usb_state_context = NULL;
static bool furi_hal_usb_locked = false;
static FuriHalUsbHidConfig s_hid_config;
static bool s_hid_config_valid;

FuriHalUsbInterface usb_cdc_single = {.name = "cdc_single"};
FuriHalUsbInterface usb_cdc_dual = {.name = "cdc_dual"};
FuriHalUsbInterface usb_hid = {.name = "hid"};
FuriHalUsbInterface usb_hid_u2f = {.name = "hid_u2f"};
FuriHalUsbInterface usb_ccid = {.name = "ccid"};

void furi_hal_usb_init(void) {
    furi_hal_usb_current = NULL;
    furi_hal_usb_locked = false;

    /* The internal USB PHY mux is an RTC-domain setting that survives a soft
     * reset. If a prior boot's on-demand composite (qFlipper / USB-Storage)
     * left it routed to the OTG controller, a normal reboot would keep
     * USB-Serial-JTAG disconnected and esptool couldn't flash without the
     * manual BOOT+RESET dance. Force the mux back to USJ here at boot; the
     * on-demand composite installs re-route to OTG when actually needed. */
    furi_hal_usb_composite_restore_serial_jtag();
}

bool furi_hal_usb_set_config(FuriHalUsbInterface* new_if, void* ctx) {
    if(furi_hal_usb_locked) {
        return false;
    }

    FuriHalUsbInterface* prev = furi_hal_usb_current;
    if(prev == &usb_hid && new_if != &usb_hid) {
        furi_hal_usb_hid_backend_stop();
    }
#if CONFIG_IDF_TARGET_ESP32S3
    if(prev == &usb_hid_u2f && new_if != &usb_hid_u2f) {
        if(!furi_hal_usb_u2f_backend_stop()) return false;
    }
#endif

    if(new_if == &usb_hid) {
        const FuriHalUsbHidConfig* cfg = ctx ? ctx : (s_hid_config_valid ? &s_hid_config : NULL);
        if(!furi_hal_usb_hid_backend_start(cfg)) {
#if CONFIG_IDF_TARGET_ESP32S3
            if(prev == &usb_hid_u2f) {
                if(!furi_hal_usb_u2f_backend_start()) {
                    FURI_LOG_E("FuriHalUsb", "Failed to restore U2F after HID start failure");
                }
            }
#endif
            return false;
        }
        if(ctx) {
            memcpy(&s_hid_config, ctx, sizeof(s_hid_config));
            s_hid_config_valid = true;
        }
    }
#if CONFIG_IDF_TARGET_ESP32S3
    if(new_if == &usb_hid_u2f && !furi_hal_usb_u2f_backend_start()) {
        if(prev == &usb_hid) {
            const FuriHalUsbHidConfig* cfg = s_hid_config_valid ? &s_hid_config : NULL;
            if(!furi_hal_usb_hid_backend_start(cfg)) {
                FURI_LOG_E("FuriHalUsb", "Failed to restore HID after U2F start failure");
            }
        }
        return false;
    }
#else
    if(new_if == &usb_hid_u2f) {
        if(prev == &usb_hid) {
            const FuriHalUsbHidConfig* cfg = s_hid_config_valid ? &s_hid_config : NULL;
            if(!furi_hal_usb_hid_backend_start(cfg)) {
                FURI_LOG_E("FuriHalUsb", "Failed to restore HID after unsupported U2F request");
            }
        }
        return false;
    }
#endif

    furi_hal_usb_current = new_if;

    if(furi_hal_usb_state_callback) {
        furi_hal_usb_state_callback(
            FuriHalUsbStateEventDescriptorRequest, furi_hal_usb_state_context);
    }

    return true;
}

FuriHalUsbInterface* furi_hal_usb_get_config(void) {
    return furi_hal_usb_current;
}

bool furi_hal_usb_hid_release_stack(void) {
    if(furi_hal_usb_current == &usb_hid) {
        FURI_LOG_E("FuriHalUsb", "Cannot release the HID stack while USB HID is active");
        return false;
    }
    return furi_hal_usb_hid_backend_release_stack();
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
#if CONFIG_IDF_TARGET_ESP32S3
    else if(furi_hal_usb_current == &usb_hid_u2f) {
        if(!furi_hal_usb_u2f_backend_stop()) {
            FURI_LOG_E("FuriHalUsb", "Failed to stop U2F USB backend");
        }
    }
#endif
}

void furi_hal_usb_enable(void) {
    if(furi_hal_usb_current == &usb_hid) {
        const FuriHalUsbHidConfig* cfg = s_hid_config_valid ? &s_hid_config : NULL;
        furi_hal_usb_hid_backend_start(cfg);
    }
#if CONFIG_IDF_TARGET_ESP32S3
    else if(furi_hal_usb_current == &usb_hid_u2f) {
        if(!furi_hal_usb_u2f_backend_start()) {
            FURI_LOG_E("FuriHalUsb", "Failed to restart U2F USB backend");
        }
    }
#endif
}

void furi_hal_usb_set_state_callback(FuriHalUsbStateCallback cb, void* ctx) {
    furi_hal_usb_state_callback = cb;
    furi_hal_usb_state_context = ctx;
}

void furi_hal_usb_reinit(void) {
}
