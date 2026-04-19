#include "furi_hal_usb.h"

#include <stddef.h>

static FuriHalUsbInterface* furi_hal_usb_current = NULL;
static FuriHalUsbStateCallback furi_hal_usb_state_callback = NULL;
static void* furi_hal_usb_state_context = NULL;
static HidStateCallback furi_hal_hid_state_callback = NULL;
static void* furi_hal_hid_state_context = NULL;
static bool furi_hal_usb_locked = false;
static bool furi_hal_hid_connected = false;

FuriHalUsbInterface usb_cdc_single = {.name = "cdc_single"};
FuriHalUsbInterface usb_cdc_dual = {.name = "cdc_dual"};
FuriHalUsbInterface usb_hid = {.name = "hid"};
FuriHalUsbInterface usb_hid_u2f = {.name = "hid_u2f"};
FuriHalUsbInterface usb_ccid = {.name = "ccid"};

static void furi_hal_hid_publish_state(bool connected) {
    furi_hal_hid_connected = connected;
    if(furi_hal_hid_state_callback) {
        furi_hal_hid_state_callback(connected, furi_hal_hid_state_context);
    }
}

void furi_hal_usb_init(void) {
    furi_hal_usb_current = NULL;
    furi_hal_usb_locked = false;
    furi_hal_hid_publish_state(false);
}

bool furi_hal_usb_set_config(FuriHalUsbInterface* new_if, void* ctx) {
    (void)ctx;

    if(furi_hal_usb_locked) {
        return false;
    }

    furi_hal_usb_current = new_if;
    furi_hal_hid_publish_state(new_if == &usb_hid);

    if(furi_hal_usb_state_callback) {
        furi_hal_usb_state_callback(FuriHalUsbStateEventDescriptorRequest, furi_hal_usb_state_context);
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
    furi_hal_hid_publish_state(false);
}

void furi_hal_usb_enable(void) {
    furi_hal_hid_publish_state(furi_hal_usb_current == &usb_hid);
}

void furi_hal_usb_set_state_callback(FuriHalUsbStateCallback cb, void* ctx) {
    furi_hal_usb_state_callback = cb;
    furi_hal_usb_state_context = ctx;
}

void furi_hal_usb_reinit(void) {
    furi_hal_hid_publish_state(furi_hal_usb_current == &usb_hid);
}

bool furi_hal_hid_is_connected(void) {
    return furi_hal_hid_connected;
}

uint8_t furi_hal_hid_get_led_state(void) {
    return 0;
}

void furi_hal_hid_set_state_callback(HidStateCallback cb, void* ctx) {
    furi_hal_hid_state_callback = cb;
    furi_hal_hid_state_context = ctx;

    if(furi_hal_hid_state_callback) {
        furi_hal_hid_state_callback(furi_hal_hid_connected, furi_hal_hid_state_context);
    }
}

bool furi_hal_hid_kb_press(uint16_t button) {
    (void)button;
    return furi_hal_hid_connected;
}

bool furi_hal_hid_kb_release(uint16_t button) {
    (void)button;
    return furi_hal_hid_connected;
}

bool furi_hal_hid_kb_release_all(void) {
    return furi_hal_hid_connected;
}

bool furi_hal_hid_mouse_move(int8_t dx, int8_t dy) {
    (void)dx;
    (void)dy;
    return furi_hal_hid_connected;
}

bool furi_hal_hid_mouse_press(uint8_t button) {
    (void)button;
    return furi_hal_hid_connected;
}

bool furi_hal_hid_mouse_release(uint8_t button) {
    (void)button;
    return furi_hal_hid_connected;
}

bool furi_hal_hid_mouse_scroll(int8_t delta) {
    (void)delta;
    return furi_hal_hid_connected;
}

bool furi_hal_hid_consumer_key_press(uint16_t button) {
    (void)button;
    return furi_hal_hid_connected;
}

bool furi_hal_hid_consumer_key_release(uint16_t button) {
    (void)button;
    return furi_hal_hid_connected;
}

bool furi_hal_hid_consumer_key_release_all(void) {
    return furi_hal_hid_connected;
}
