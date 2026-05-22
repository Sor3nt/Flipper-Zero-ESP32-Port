#pragma once

#include <gui/view.h>
#include "desktop_events.h"

typedef enum {
    DesktopUsbStorageStateInit,
    DesktopUsbStorageStateActive,
    DesktopUsbStorageStateError,
} DesktopUsbStorageState;

typedef struct DesktopUsbStorageView DesktopUsbStorageView;

typedef void (*DesktopUsbStorageViewCallback)(DesktopEvent event, void* context);

struct DesktopUsbStorageView {
    View* view;
    DesktopUsbStorageViewCallback callback;
    void* context;
};

typedef struct {
    DesktopUsbStorageState state;
    const char* error_msg;
} DesktopUsbStorageViewModel;

void desktop_usb_storage_set_callback(
    DesktopUsbStorageView* usb_storage,
    DesktopUsbStorageViewCallback callback,
    void* context);

void desktop_usb_storage_set_state(
    DesktopUsbStorageView* usb_storage,
    DesktopUsbStorageState state,
    const char* error_msg);

View* desktop_usb_storage_get_view(DesktopUsbStorageView* usb_storage);
DesktopUsbStorageView* desktop_usb_storage_alloc(void);
void desktop_usb_storage_free(DesktopUsbStorageView* usb_storage);
