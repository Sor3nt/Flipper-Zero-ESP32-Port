/**
 * USB-Storage scene — exposes the SD card as a USB Mass Storage device.
 *
 * This is the former standalone "USB-Storage" app turned into a desktop scene.
 * It deliberately stays full-screen and blocking (not a background toggle):
 * while active the SD card is detached from the firmware and handed to the
 * host, so no other app could use storage anyway.
 *
 * Mutual exclusion: the qFlipper bridge shares the same TinyUSB composite, so
 * on enter we stop it first.
 *
 * Only the ESP32-S3 / S2 path has USB-OTG; on other targets the scene shows an
 * error.
 */

#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>

#include "../desktop_i.h"
#include "../views/desktop_view_usb_storage.h"
#include "../helpers/qflipper_bridge.h"
#include "desktop_scene.h"

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2
#include "furi_hal_usb_tinyusb_composite.h"
#include "furi_hal_usb_msc.h"
#include "furi_hal_sd.h"
#define USB_STORAGE_HAVE_USB 1
#else
#define USB_STORAGE_HAVE_USB 0
#endif

#define TAG "DesktopUsbStorage"

/* Whether the SD was mounted before we took it over — restored on leave.
 * Single-instance scene, so a file-static is fine. */
static bool s_sd_was_mounted = false;

void desktop_scene_usb_storage_callback(DesktopEvent event, void* context) {
    Desktop* desktop = (Desktop*)context;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, event);
}

#if USB_STORAGE_HAVE_USB
static bool desktop_usb_storage_enter(Desktop* desktop, const char** error_msg) {
    Storage* storage = desktop->storage;

    /* 1) Unmount on the storage-service side — closes any open file handles. */
    s_sd_was_mounted = (storage_sd_status(storage) == FSE_OK);
    if(s_sd_was_mounted) {
        FS_Error err = storage_sd_unmount(storage);
        if(err != FSE_OK) {
            FURI_LOG_W(TAG, "storage_sd_unmount returned %d (continuing)", err);
        }
    }

    /* 2) Release FATFS in the HAL but keep sdmmc_card_t alive for MSC. */
    if(!furi_hal_sd_release_fatfs()) {
        *error_msg = "FATFS release failed";
        return false;
    }

    /* 3) Ensure the composite (HID + CDC + MSC) is installed. */
    if(!furi_hal_usb_composite_install(0, 0, NULL, NULL)) {
        *error_msg = "USB composite install failed";
        return false;
    }

    /* 4) Flip MSC active — host sees a removable disk. */
    if(!furi_hal_usb_msc_start()) {
        *error_msg = "MSC start failed";
        return false;
    }

    return true;
}

static void desktop_usb_storage_leave(Desktop* desktop) {
    /* stop() arms the SCSI sense to MEDIUM_NOT_PRESENT; wait long enough for
     * the host's ~1 s TEST UNIT READY poll so it unmounts before we re-attach
     * the SD on the firmware side. */
    furi_hal_usb_msc_stop();
    furi_delay_ms(1500);

    if(s_sd_was_mounted) {
        FS_Error err = storage_sd_mount(desktop->storage);
        if(err != FSE_OK) {
            FURI_LOG_E(TAG, "storage_sd_mount after MSC failed: %d", err);
        }
        s_sd_was_mounted = false;
    }
}
#endif /* USB_STORAGE_HAVE_USB */

void desktop_scene_usb_storage_on_enter(void* context) {
    Desktop* desktop = (Desktop*)context;

    desktop_usb_storage_set_callback(
        desktop->usb_storage_view, desktop_scene_usb_storage_callback, desktop);

#if USB_STORAGE_HAVE_USB
    /* Mutual exclusion: the qFlipper bridge holds the shared composite. */
    qflipper_bridge_stop();

    desktop_usb_storage_set_state(desktop->usb_storage_view, DesktopUsbStorageStateInit, NULL);
    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdUsbStorage);

    const char* error_msg = NULL;
    if(desktop_usb_storage_enter(desktop, &error_msg)) {
        desktop_usb_storage_set_state(
            desktop->usb_storage_view, DesktopUsbStorageStateActive, NULL);
    } else {
        desktop_usb_storage_set_state(
            desktop->usb_storage_view, DesktopUsbStorageStateError, error_msg);
    }
#else
    desktop_usb_storage_set_state(
        desktop->usb_storage_view,
        DesktopUsbStorageStateError,
        "USB-Storage nicht\nverfügbar auf\ndiesem Board");
    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdUsbStorage);
#endif
}

bool desktop_scene_usb_storage_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = (Desktop*)context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == DesktopUsbStorageEventExit) {
            scene_manager_search_and_switch_to_previous_scene(
                desktop->scene_manager, DesktopSceneMain);
            consumed = true;
        }
    }

    return consumed;
}

void desktop_scene_usb_storage_on_exit(void* context) {
#if USB_STORAGE_HAVE_USB
    Desktop* desktop = (Desktop*)context;
    desktop_usb_storage_leave(desktop);
#else
    UNUSED(context);
#endif
}
