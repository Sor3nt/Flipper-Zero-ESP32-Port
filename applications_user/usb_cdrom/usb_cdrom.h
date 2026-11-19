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

// CD-ROM sector size is 2048 bytes (Mode 1 user data).
#define CD_BLOCK_SIZE 2048u
// File offset limit: storage_file_seek() ends in fseek(long), and long is 32-bit
// here, so every byte offset must stay below 2 GiB (an offset >= 0x80000000 turns
// negative and the seek fails -> MEDIUM ERROR for the rest of the disc).
#define CD_MAX_BLOCKS 1048575u // (2 GiB / 2048) - 1 -> just under 2 GiB
#define CD_MAX_SIZE   ((uint64_t)CD_MAX_BLOCKS * CD_BLOCK_SIZE)

#define USB_CDROM_APP_FOLDER APP_DATA_PATH("")
#define USB_CDROM_IMAGE_EXT  ".iso"
#define USB_CDROM_NAME_MAX   64

#define USB_CDROM_EJECT_WAIT_MS 1500
#define USB_CDROM_RELEASE_TRIES 5
#define USB_CDROM_RELEASE_RETRY_MS 100

typedef enum {
    UsbCdromStateSelect, // idle: press OK to pick an ISO
    UsbCdromStateBadImage, // not a file / size not a multiple of 2048 / too large
    UsbCdromStateUsbBusy, // USB lease refused
    UsbCdromStateError, // generic mount failure
    UsbCdromStateWork, // mounted — host sees the CD-ROM
    UsbCdromStateEjectWarn, // Back pressed while the host still holds the medium
} UsbCdromState;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* event_queue;
    FuriMutex* mutex;
    DialogsApp* dialogs;
    NotificationApp* notifications;
    Storage* storage;

    UsbCdromState state;
    // PORT: which step failed, shown on the error screen (was a bare "Failed").
    const char* error_reason;
    FuriString* image_path;
    char image_name[USB_CDROM_NAME_MAX];
    File* file; // open ISO; read by the backing callback on the TinyUSB task
    uint32_t image_size; // bytes
    uint32_t block_count; // image_size / 2048

    volatile uint32_t bytes_read;
    volatile uint32_t last_activity_tick;

    FuriHalUsbInterface* usb_previous;
    bool usb_lease;
    bool usb_composite;
    bool msc_active;
    bool sleep_inhibited;

    bool running;
} UsbCdromApp;
