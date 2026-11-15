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

#define MASS_BLOCK_SIZE 512u
// 32-bit file offset limit: (block_count * 512) must fit in uint32_t.
#define MASS_MAX_SIZE   0xFFFFFE00u // < 4 GB, multiple of 512

#define MASS_STORAGE_APP_FOLDER APP_DATA_PATH("")
#define MASS_STORAGE_IMAGE_EXT  ".img"
#define MASS_STORAGE_NAME_MAX   64

#define MASS_EJECT_WAIT_MS 1500
#define MASS_RELEASE_TRIES 5
#define MASS_RELEASE_RETRY_MS 100

typedef enum {
    MassSelectModeOpen, // "Select image" — browse for an existing .img
    MassSelectModeNew, // "New image" — create one of the chosen size
    MassSelectModeCount,
} MassSelectMode;

typedef enum {
    MassStateSelect, // idle menu
    MassStateCreating, // image being created (blocking)
    MassStateBadImage, // not a file / size not a multiple of 512 / too large
    MassStateUsbBusy, // USB lease refused
    MassStateError, // generic failure
    MassStateWork, // mounted — host sees the drive
    MassStateEjectWarn, // Back pressed while the host still holds the medium
} MassState;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* event_queue;
    FuriMutex* mutex;
    DialogsApp* dialogs;
    NotificationApp* notifications;
    Storage* storage;

    MassState state;
    // PORT: which step failed, shown on the error screen (was a bare "Failed").
    const char* error_reason;
    MassSelectMode select_mode;
    uint8_t new_size_index; // index into the preset size table

    FuriString* image_path;
    char image_name[MASS_STORAGE_NAME_MAX];
    File* file; // open image; used by the backing callbacks on the TinyUSB task
    uint32_t image_size; // bytes
    uint32_t block_count;

    volatile uint32_t bytes_read;
    volatile uint32_t bytes_written;
    volatile uint32_t last_activity_tick;

    FuriHalUsbInterface* usb_previous;
    bool usb_lease;
    bool usb_composite;
    bool msc_active;
    bool sleep_inhibited;

    bool running;
} MassStorageApp;
