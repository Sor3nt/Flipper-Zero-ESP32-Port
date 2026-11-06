#include "furi_hal_usb.h"
#include "furi_hal_usb_hid_backend.h"
#include "furi_hal_usb_tinyusb_composite.h"

#include <furi.h>
#include <stddef.h>

#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "furi_hal_usb_hid_u2f.h"
#endif

#define TAG "FuriHalUsb"

static FuriHalUsbInterface* furi_hal_usb_current = NULL;
static FuriHalUsbStateCallback furi_hal_usb_state_callback = NULL;
static void* furi_hal_usb_state_context = NULL;
static FuriHalUsbU2fPreemptionCallback furi_hal_usb_u2f_preemption_callback = NULL;
static void* furi_hal_usb_u2f_preemption_context = NULL;
static bool furi_hal_usb_locked = false;
static FuriMutex* furi_hal_usb_mutex;
static const void* furi_hal_usb_lease_owner;
static bool furi_hal_usb_lease_preempted_composite;
static bool furi_hal_usb_preempting;
static FuriHalUsbHidConfig furi_hal_usb_hid_config;
#if CONFIG_IDF_TARGET_ESP32S3
static bool furi_hal_usb_u2f_preempted_composite;
static FuriHalUsbInterface* furi_hal_usb_u2f_previous;
#endif

FuriHalUsbInterface usb_cdc_single = {.name = "cdc_single"};
FuriHalUsbInterface usb_cdc_dual = {.name = "cdc_dual"};
FuriHalUsbInterface usb_hid = {.name = "hid"};
FuriHalUsbInterface usb_hid_u2f = {.name = "hid_u2f"};
FuriHalUsbInterface usb_ccid = {.name = "ccid"};

void furi_hal_usb_init(void) {
    if(!furi_hal_usb_mutex) {
        furi_hal_usb_mutex = furi_mutex_alloc(FuriMutexTypeRecursive);
        furi_check(furi_hal_usb_mutex);
    }
    furi_hal_usb_current = NULL;
    furi_hal_usb_locked = false;
    furi_hal_usb_lease_owner = NULL;
    furi_hal_usb_lease_preempted_composite = false;
    furi_hal_usb_preempting = false;
    furi_hal_usb_hid_config = (FuriHalUsbHidConfig){0};
#if CONFIG_IDF_TARGET_ESP32S3
    furi_hal_usb_u2f_preempted_composite = false;
    furi_hal_usb_u2f_previous = NULL;
#endif

    /* The internal USB PHY mux is an RTC-domain setting that survives a soft
     * reset. If a prior boot's on-demand composite (qFlipper / USB-Storage)
     * left it routed to the OTG controller, a normal reboot would keep
     * USB-Serial-JTAG disconnected and esptool couldn't flash without the
     * manual BOOT+RESET dance. Force the mux back to USJ here at boot; the
     * on-demand composite installs re-route to OTG when actually needed. */
    furi_hal_usb_composite_restore_serial_jtag();
}

static bool furi_hal_usb_set_config_internal(
    FuriHalUsbInterface* new_if,
    void* ctx,
    const void* owner) {
    if(furi_hal_usb_lease_owner != owner) return false;

#if !CONFIG_IDF_TARGET_ESP32S3
    if(new_if == &usb_hid_u2f) {
        return false;
    }
#endif

    FuriHalUsbInterface* prev = furi_hal_usb_current;
#if CONFIG_IDF_TARGET_ESP32S3
    bool preempted_composite = false;
    if(new_if == &usb_hid_u2f && prev != &usb_hid_u2f &&
       furi_hal_usb_composite_is_installed()) {
        if(!furi_hal_usb_u2f_preemption_callback ||
           !furi_hal_usb_u2f_preemption_callback(
               true, furi_hal_usb_u2f_preemption_context)) {
            FURI_LOG_E(TAG, "Active USB composite cannot be preempted for U2F");
            return false;
        }
        if(furi_hal_usb_composite_is_installed()) {
            FURI_LOG_E(TAG, "USB composite owner did not release its interface for U2F");
            if(!furi_hal_usb_u2f_preemption_callback(
                   false, furi_hal_usb_u2f_preemption_context)) {
                FURI_LOG_E(TAG, "Failed to resume the composite owner after rejected U2F handoff");
            }
            return false;
        }
        furi_hal_usb_u2f_preempted_composite = true;
        preempted_composite = true;
    }

    if(prev == &usb_hid_u2f && new_if != &usb_hid_u2f &&
       furi_hal_usb_u2f_preempted_composite &&
       new_if != furi_hal_usb_u2f_previous) {
        FURI_LOG_E(TAG, "Cannot switch from U2F while a preempted USB owner must be restored");
        return false;
    }
#endif

#if CONFIG_IDF_TARGET_ESP32S3
    if(prev == &usb_hid_u2f && new_if != &usb_hid_u2f) {
        if(!furi_hal_usb_u2f_backend_stop()) {
            return false;
        }
    }
#endif
    if(prev == &usb_hid && new_if != &usb_hid) {
        furi_hal_usb_hid_backend_stop();
    }

    bool started = true;
    if(new_if == &usb_hid) {
        started = furi_hal_usb_hid_backend_start((const FuriHalUsbHidConfig*)ctx);
    }
#if CONFIG_IDF_TARGET_ESP32S3
    else if(new_if == &usb_hid_u2f) {
        started = furi_hal_usb_u2f_backend_start();
    }
#endif

    if(!started) {
#if CONFIG_IDF_TARGET_ESP32S3
        if(prev == &usb_hid_u2f && new_if != &usb_hid_u2f) {
            if(!furi_hal_usb_u2f_backend_start()) {
                FURI_LOG_E(TAG, "Failed to restore the previous U2F USB mode");
                furi_hal_usb_current = NULL;
                if(furi_hal_usb_u2f_preempted_composite) {
                    if(!furi_hal_usb_u2f_preemption_callback(
                           false, furi_hal_usb_u2f_preemption_context)) {
                        FURI_LOG_E(TAG, "Failed to resume the composite owner after U2F rollback");
                    }
                    furi_hal_usb_u2f_preempted_composite = false;
                    furi_hal_usb_u2f_previous = NULL;
                }
                return false;
            }
        }
#endif
        if(prev == &usb_hid && new_if != &usb_hid &&
           !furi_hal_usb_hid_backend_start(&furi_hal_usb_hid_config)) {
            FURI_LOG_E(TAG, "Failed to restore the previous HID USB mode");
            furi_hal_usb_current = NULL;
#if CONFIG_IDF_TARGET_ESP32S3
            if(preempted_composite) {
                if(!furi_hal_usb_u2f_preemption_callback(
                       false, furi_hal_usb_u2f_preemption_context)) {
                    FURI_LOG_E(TAG, "Failed to resume the composite owner after HID rollback");
                }
                furi_hal_usb_u2f_preempted_composite = false;
                furi_hal_usb_u2f_previous = NULL;
            }
#endif
            return false;
        }
#if CONFIG_IDF_TARGET_ESP32S3
        if(preempted_composite) {
            if(!furi_hal_usb_u2f_preemption_callback(
                   false, furi_hal_usb_u2f_preemption_context)) {
                FURI_LOG_E(TAG, "Failed to resume the composite owner after U2F start failure");
            }
            furi_hal_usb_u2f_preempted_composite = false;
            furi_hal_usb_u2f_previous = NULL;
        }
#endif
        return false;
    }

    furi_hal_usb_current = new_if;
#if CONFIG_IDF_TARGET_ESP32S3
    if(new_if == &usb_hid_u2f) {
        furi_hal_usb_u2f_previous = prev;
    }
#endif
    if(new_if == &usb_hid) {
        furi_hal_usb_hid_config =
            ctx ? *(const FuriHalUsbHidConfig*)ctx : (FuriHalUsbHidConfig){0};
    }
#if CONFIG_IDF_TARGET_ESP32S3
    if(prev == &usb_hid_u2f && new_if != &usb_hid_u2f &&
       furi_hal_usb_u2f_preempted_composite) {
        if(!furi_hal_usb_u2f_preemption_callback(
               false, furi_hal_usb_u2f_preemption_context)) {
            FURI_LOG_E(TAG, "Failed to restore the USB owner that was paused for U2F");
        }
        furi_hal_usb_u2f_preempted_composite = false;
    }
    if(prev == &usb_hid_u2f && new_if != &usb_hid_u2f) {
        furi_hal_usb_u2f_previous = NULL;
    }
#endif

    if(furi_hal_usb_state_callback) {
        furi_hal_usb_state_callback(
            FuriHalUsbStateEventDescriptorRequest, furi_hal_usb_state_context);
    }

    return true;
}

bool furi_hal_usb_operation_start(const void* owner) {
    if(!furi_hal_usb_mutex ||
       furi_mutex_acquire(furi_hal_usb_mutex, FuriWaitForever) != FuriStatusOk) {
        return false;
    }

    const bool owner_matches = furi_hal_usb_lease_owner == owner;
    const bool preemption_operation = furi_hal_usb_preempting && owner == NULL;
    if((!owner_matches && !preemption_operation) ||
       (furi_hal_usb_locked && !owner && !preemption_operation)) {
        furi_mutex_release(furi_hal_usb_mutex);
        return false;
    }
    return true;
}

void furi_hal_usb_operation_finish(void) {
    furi_assert(furi_hal_usb_mutex);
    furi_check(furi_mutex_release(furi_hal_usb_mutex) == FuriStatusOk);
}

bool furi_hal_usb_set_config(FuriHalUsbInterface* new_if, void* ctx) {
    if(!furi_hal_usb_operation_start(NULL)) return false;
    const bool result = furi_hal_usb_set_config_internal(new_if, ctx, NULL);
    furi_hal_usb_operation_finish();
    return result;
}

bool furi_hal_usb_set_config_for_owner(
    const void* owner,
    FuriHalUsbInterface* new_if,
    void* ctx) {
    if(!owner || !furi_hal_usb_operation_start(owner)) return false;
    const bool result = furi_hal_usb_set_config_internal(new_if, ctx, owner);
    furi_hal_usb_operation_finish();
    return result;
}

FuriHalUsbInterface* furi_hal_usb_get_config(void) {
    if(!furi_hal_usb_mutex ||
       furi_mutex_acquire(furi_hal_usb_mutex, FuriWaitForever) != FuriStatusOk) {
        return NULL;
    }
    FuriHalUsbInterface* current = furi_hal_usb_current;
    furi_mutex_release(furi_hal_usb_mutex);
    return current;
}

bool furi_hal_usb_lease_acquire(const void* owner, FuriHalUsbInterface** previous) {
    if(!owner || !previous || !furi_hal_usb_mutex ||
       furi_mutex_acquire(furi_hal_usb_mutex, FuriWaitForever) != FuriStatusOk) {
        return false;
    }
    if(furi_hal_usb_lease_owner || furi_hal_usb_locked) {
        furi_mutex_release(furi_hal_usb_mutex);
        return false;
    }

    furi_hal_usb_lease_owner = owner;
    furi_hal_usb_lease_preempted_composite = false;
    if(furi_hal_usb_composite_is_installed()) {
        if(!furi_hal_usb_u2f_preemption_callback) {
            furi_hal_usb_lease_owner = NULL;
            furi_mutex_release(furi_hal_usb_mutex);
            return false;
        }

        furi_hal_usb_preempting = true;
        const bool released = furi_hal_usb_u2f_preemption_callback(
            true, furi_hal_usb_u2f_preemption_context);
        furi_hal_usb_preempting = false;
        if(!released || furi_hal_usb_composite_is_installed()) {
            if(released) {
                furi_hal_usb_preempting = true;
                if(!furi_hal_usb_u2f_preemption_callback(
                       false, furi_hal_usb_u2f_preemption_context)) {
                    FURI_LOG_E(TAG, "Failed to resume USB owner after rejected lease");
                }
                furi_hal_usb_preempting = false;
            }
            furi_hal_usb_lease_owner = NULL;
            furi_mutex_release(furi_hal_usb_mutex);
            return false;
        }
        furi_hal_usb_lease_preempted_composite = true;
    }

    *previous = furi_hal_usb_current;
    furi_mutex_release(furi_hal_usb_mutex);
    return true;
}

bool furi_hal_usb_lease_release(const void* owner, FuriHalUsbInterface* previous) {
    if(!owner || !furi_hal_usb_mutex ||
       furi_mutex_acquire(furi_hal_usb_mutex, FuriWaitForever) != FuriStatusOk) {
        return false;
    }
    if(furi_hal_usb_lease_owner != owner) {
        furi_mutex_release(furi_hal_usb_mutex);
        return false;
    }
    if(furi_hal_usb_composite_is_installed()) {
        FURI_LOG_E(TAG, "Cannot release USB lease while its composite is still installed");
        furi_mutex_release(furi_hal_usb_mutex);
        return false;
    }

    if(furi_hal_usb_current != previous &&
       !furi_hal_usb_set_config_internal(previous, NULL, owner)) {
        FURI_LOG_E(TAG, "Failed to restore USB interface before releasing lease");
        furi_mutex_release(furi_hal_usb_mutex);
        return false;
    }

    bool resumed = true;
    if(furi_hal_usb_lease_preempted_composite) {
        furi_hal_usb_preempting = true;
        resumed = furi_hal_usb_u2f_preemption_callback &&
                  furi_hal_usb_u2f_preemption_callback(
                      false, furi_hal_usb_u2f_preemption_context);
        furi_hal_usb_preempting = false;
        if(!resumed) FURI_LOG_E(TAG, "Failed to resume preempted USB composite owner");
    }

    furi_hal_usb_lease_preempted_composite = false;
    furi_hal_usb_lease_owner = NULL;
    furi_mutex_release(furi_hal_usb_mutex);
    return resumed;
}

bool furi_hal_usb_hid_release_for_owner(const void* owner) {
    if(!owner || !furi_hal_usb_operation_start(owner)) return false;
    bool released = false;
    /* Never pull the stack from under an interface that is still selected. */
    if(furi_hal_usb_current != &usb_hid
#if CONFIG_IDF_TARGET_ESP32S3
       && furi_hal_usb_current != &usb_hid_u2f
#endif
    ) {
        released = furi_hal_usb_hid_backend_release_stack();
    }
    furi_hal_usb_operation_finish();
    return released;
}

bool furi_hal_usb_is_lease_owner(const void* owner) {
    if(!furi_hal_usb_mutex ||
       furi_mutex_acquire(furi_hal_usb_mutex, FuriWaitForever) != FuriStatusOk) {
        return false;
    }
    const bool is_owner = owner && furi_hal_usb_lease_owner == owner;
    furi_mutex_release(furi_hal_usb_mutex);
    return is_owner;
}

bool furi_hal_usb_is_preempting(void) {
    if(!furi_hal_usb_mutex ||
       furi_mutex_acquire(furi_hal_usb_mutex, FuriWaitForever) != FuriStatusOk) {
        return false;
    }
    const bool preempting = furi_hal_usb_preempting;
    furi_mutex_release(furi_hal_usb_mutex);
    return preempting;
}

void furi_hal_usb_lock(void) {
    if(!furi_hal_usb_mutex) return;
    furi_check(furi_mutex_acquire(furi_hal_usb_mutex, FuriWaitForever) == FuriStatusOk);
    furi_hal_usb_locked = true;
    furi_check(furi_mutex_release(furi_hal_usb_mutex) == FuriStatusOk);
}

void furi_hal_usb_unlock(void) {
    if(!furi_hal_usb_mutex) return;
    furi_check(furi_mutex_acquire(furi_hal_usb_mutex, FuriWaitForever) == FuriStatusOk);
    furi_hal_usb_locked = false;
    furi_check(furi_mutex_release(furi_hal_usb_mutex) == FuriStatusOk);
}

bool furi_hal_usb_is_locked(void) {
    if(!furi_hal_usb_mutex) return false;
    furi_check(furi_mutex_acquire(furi_hal_usb_mutex, FuriWaitForever) == FuriStatusOk);
    const bool locked = furi_hal_usb_locked;
    furi_check(furi_mutex_release(furi_hal_usb_mutex) == FuriStatusOk);
    return locked;
}

void furi_hal_usb_disable(void) {
    if(!furi_hal_usb_operation_start(NULL)) return;
    if(furi_hal_usb_current == &usb_hid) {
        furi_hal_usb_hid_backend_stop();
    }
#if CONFIG_IDF_TARGET_ESP32S3
    else if(furi_hal_usb_current == &usb_hid_u2f) {
        if(!furi_hal_usb_u2f_backend_stop()) {
            FURI_LOG_E(TAG, "Failed to disable the U2F USB backend");
        }
    }
#endif
    furi_hal_usb_operation_finish();
}

void furi_hal_usb_enable(void) {
    if(!furi_hal_usb_operation_start(NULL)) return;
    if(furi_hal_usb_current == &usb_hid) {
        if(!furi_hal_usb_hid_backend_start(&furi_hal_usb_hid_config)) {
            FURI_LOG_E(TAG, "Failed to enable the HID USB backend");
        }
    }
#if CONFIG_IDF_TARGET_ESP32S3
    else if(furi_hal_usb_current == &usb_hid_u2f) {
        if(!furi_hal_usb_u2f_backend_start()) {
            FURI_LOG_E(TAG, "Failed to enable the U2F USB backend");
        }
    }
#endif
    furi_hal_usb_operation_finish();
}

void furi_hal_usb_set_state_callback(FuriHalUsbStateCallback cb, void* ctx) {
    if(!furi_hal_usb_mutex) return;
    furi_check(furi_mutex_acquire(furi_hal_usb_mutex, FuriWaitForever) == FuriStatusOk);
    furi_hal_usb_state_callback = cb;
    furi_hal_usb_state_context = ctx;
    furi_check(furi_mutex_release(furi_hal_usb_mutex) == FuriStatusOk);
}

void furi_hal_usb_set_u2f_preemption_callback(
    FuriHalUsbU2fPreemptionCallback cb,
    void* ctx) {
    if(!furi_hal_usb_mutex) return;
    furi_check(furi_mutex_acquire(furi_hal_usb_mutex, FuriWaitForever) == FuriStatusOk);
    furi_hal_usb_u2f_preemption_callback = cb;
    furi_hal_usb_u2f_preemption_context = ctx;
    furi_check(furi_mutex_release(furi_hal_usb_mutex) == FuriStatusOk);
}

static void furi_hal_usb_reinit_internal(void) {
    if(furi_hal_usb_current == &usb_hid || furi_hal_usb_composite_is_installed()) {
        if(!furi_hal_usb_backend_force_reenumeration()) {
            FURI_LOG_E(TAG, "Failed to re-enumerate the USB device");
        }
    }
#if CONFIG_IDF_TARGET_ESP32S3
    else if(furi_hal_usb_current == &usb_hid_u2f) {
        if(!furi_hal_usb_backend_force_reenumeration()) {
            FURI_LOG_E(TAG, "Failed to re-enumerate the U2F device");
        }
    }
#endif
}

void furi_hal_usb_reinit(void) {
    if(!furi_hal_usb_operation_start(NULL)) return;
    furi_hal_usb_reinit_internal();
    furi_hal_usb_operation_finish();
}

void furi_hal_usb_reinit_for_owner(const void* owner) {
    if(!owner || !furi_hal_usb_operation_start(owner)) return;
    furi_hal_usb_reinit_internal();
    furi_hal_usb_operation_finish();
}
