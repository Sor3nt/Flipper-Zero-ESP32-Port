#pragma once

#include <stdbool.h>

#include "furi_hal_usb_hid.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FuriHalUsbInterface FuriHalUsbInterface;

struct FuriHalUsbInterface {
    const char* name;
};

extern FuriHalUsbInterface usb_cdc_single;
extern FuriHalUsbInterface usb_cdc_dual;
extern FuriHalUsbInterface usb_hid;
extern FuriHalUsbInterface usb_hid_u2f;
extern FuriHalUsbInterface usb_ccid;

typedef enum {
    FuriHalUsbStateEventReset,
    FuriHalUsbStateEventWakeup,
    FuriHalUsbStateEventSuspend,
    FuriHalUsbStateEventDescriptorRequest,
} FuriHalUsbStateEvent;

typedef void (*FuriHalUsbStateCallback)(FuriHalUsbStateEvent state, void* context);

void furi_hal_usb_init(void);
bool furi_hal_usb_set_config(FuriHalUsbInterface* new_if, void* ctx);
FuriHalUsbInterface* furi_hal_usb_get_config(void);
void furi_hal_usb_lock(void);
void furi_hal_usb_unlock(void);
bool furi_hal_usb_is_locked(void);
void furi_hal_usb_disable(void);
void furi_hal_usb_enable(void);
void furi_hal_usb_set_state_callback(FuriHalUsbStateCallback cb, void* ctx);
void furi_hal_usb_reinit(void);

#ifdef __cplusplus
}
#endif
