#pragma once

#include <gui/view.h>
#include "desktop_events.h"

#define HINT_TIMEOUT 2

typedef struct DesktopLockMenuView DesktopLockMenuView;

typedef void (*DesktopLockMenuViewCallback)(DesktopEvent event, void* context);

struct DesktopLockMenuView {
    View* view;
    DesktopLockMenuViewCallback callback;
    void* context;
};

typedef struct {
    uint8_t idx;
} DesktopLockMenuViewModel;

void desktop_lock_menu_set_callback(
    DesktopLockMenuView* lock_menu,
    DesktopLockMenuViewCallback callback,
    void* context);

View* desktop_lock_menu_get_view(DesktopLockMenuView* lock_menu);
void desktop_lock_menu_set_idx(DesktopLockMenuView* lock_menu, uint8_t idx);

/** Rebuild the menu items from the current toggle states and reset the
 *  selection. `usb_available` gates the qFlipper / USB-Storage entries (USB-OTG
 *  is ESP32-S3/S2 only); `bruce_available` gates the multi-boot entry. */
void desktop_lock_menu_set_states(
    DesktopLockMenuView* lock_menu,
    bool usb_available,
    bool qflipper_on,
    bool bt_on,
    bool bruce_available);

DesktopLockMenuView* desktop_lock_menu_alloc(void);
void desktop_lock_menu_free(DesktopLockMenuView* lock_menu);
