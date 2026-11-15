#pragma once

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <dialogs/dialogs.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#include <furi_hal_usb_msc.h>

// Floppy geometry — the two sizes the host (and the UFI spec) recognise.
#define FLOPPY_BLOCK_SIZE   512u
#define FLOPPY_720K_BLOCKS  1440u // 80 cyl x 2 heads x 9 sectors
#define FLOPPY_144M_BLOCKS  2880u // 80 cyl x 2 heads x 18 sectors
#define FLOPPY_720K_SIZE    (FLOPPY_720K_BLOCKS * FLOPPY_BLOCK_SIZE) // 737280
#define FLOPPY_144M_SIZE    (FLOPPY_144M_BLOCKS * FLOPPY_BLOCK_SIZE) // 1474560

#define USB_FLOPPY_APP_FOLDER   APP_DATA_PATH("")
#define USB_FLOPPY_IMAGE_EXT    ".img"
#define USB_FLOPPY_NAME_MAX     64

// How long to let the host notice the medium is gone before the composite is
// torn down (its TEST UNIT READY poll is ~1 s).
#define USB_FLOPPY_EJECT_WAIT_MS 1500
#define USB_FLOPPY_RELEASE_TRIES 5
#define USB_FLOPPY_RELEASE_RETRY_MS 100

typedef enum {
    UsbFloppyStateSelect, // idle: press OK to pick an image
    UsbFloppyStateBadImage, // not a file / wrong size
    UsbFloppyStateUsbBusy, // USB lease refused (RPC / storage / bridge)
    UsbFloppyStateError, // generic mount failure
    UsbFloppyStateWork, // mounted — host sees the floppy
    UsbFloppyStateEjectWarn, // Back pressed while the host still holds the medium
} UsbFloppyState;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* event_queue;
    FuriMutex* mutex;
    DialogsApp* dialogs;
    NotificationApp* notifications;
    Storage* storage;

    UsbFloppyState state;
    // PORT: which step failed, shown on the error screen (was a bare "Failed").
    const char* error_reason;
    FuriString* image_path;
    char image_name[USB_FLOPPY_NAME_MAX];
    File* file; // open image; used by the backing callbacks on the TinyUSB task
    uint32_t image_size; // bytes
    uint32_t block_count;
    bool write_protect; // user toggle, applied at mount

    // Stats — written on the TinyUSB task, read on the GUI thread (cosmetic).
    volatile uint32_t bytes_read;
    volatile uint32_t bytes_written;
    volatile uint32_t last_activity_tick;

    // USB ownership — each flag records something that must be given back.
    FuriHalUsbInterface* usb_previous;
    bool usb_lease;
    bool usb_composite;
    bool msc_active;
    bool sleep_inhibited;

    bool running;
} UsbFloppyApp;
