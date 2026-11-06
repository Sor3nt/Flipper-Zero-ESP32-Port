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
/* The active composite owner releases USB on acquire and resumes on release. */
typedef bool (*FuriHalUsbU2fPreemptionCallback)(bool acquire, void* context);

void furi_hal_usb_init(void);
bool furi_hal_usb_set_config(FuriHalUsbInterface* new_if, void* ctx);
bool furi_hal_usb_set_config_for_owner(
    const void* owner,
    FuriHalUsbInterface* new_if,
    void* ctx);
FuriHalUsbInterface* furi_hal_usb_get_config(void);
bool furi_hal_usb_lease_acquire(const void* owner, FuriHalUsbInterface** previous);
bool furi_hal_usb_lease_release(const void* owner, FuriHalUsbInterface* previous);
/* Give the USB pads back after an HID session: stops HID (furi_hal_usb_set_config_for_owner
 * with NULL does that), then tears the TinyUSB HID stack down and routes the shared PHY back
 * to USB-Serial-JTAG (console and flashing). Plain HID stop deliberately leaves the stack
 * installed (BadUSB re-enters it); a lease owner that is going away must not.
 * Refused (false) while HID is still the active interface, or if the stack could not be
 * released. Call it with the lease still held, before furi_hal_usb_lease_release(). */
bool furi_hal_usb_hid_release_for_owner(const void* owner);
bool furi_hal_usb_is_lease_owner(const void* owner);
bool furi_hal_usb_is_preempting(void);
bool furi_hal_usb_operation_start(const void* owner);
void furi_hal_usb_operation_finish(void);
void furi_hal_usb_lock(void);
void furi_hal_usb_unlock(void);
bool furi_hal_usb_is_locked(void);
void furi_hal_usb_disable(void);
void furi_hal_usb_enable(void);
void furi_hal_usb_reinit_for_owner(const void* owner);
void furi_hal_usb_set_state_callback(FuriHalUsbStateCallback cb, void* ctx);
void furi_hal_usb_set_u2f_preemption_callback(
    FuriHalUsbU2fPreemptionCallback cb,
    void* ctx);
void furi_hal_usb_reinit(void);

#ifdef __cplusplus
}
#endif
