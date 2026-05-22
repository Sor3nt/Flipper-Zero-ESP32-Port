/**
 * "USB-Storage" — exposes the SD card as a USB Mass Storage device to a
 * host PC, so the user can drag-and-drop files without pulling the SD out.
 *
 * Flow on entry:
 *   1) Unmount FATFS in the storage service (closes any open file handles
 *      and detaches the SD volume).
 *   2) Release FATFS in the HAL while keeping the sdmmc card alive.
 *   3) Make sure the TinyUSB composite (HID + CDC + MSC) is installed, then
 *      flip the MSC layer to "active". The host now sees a removable disk.
 *
 * The user sees a full-screen "USB-Stick aktiv" message and presses Back
 * (or ejects from the host) to return to the menu. On exit, MSC is stopped
 * and the SD is re-mounted so the firmware can use it again.
 *
 * Only the ESP32-S3 / S2 path has USB-OTG; the app is excluded from
 * Waveshare C6 builds via fam_config.py.
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/elements.h>
#include <input/input.h>
#include <storage/storage.h>

#include "sdkconfig.h"

#define TAG "UsbStorage"

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2
#include "furi_hal_usb_tinyusb_composite.h"
#include "furi_hal_usb_msc.h"
#include "furi_hal_sd.h"
#define USB_STORAGE_HAVE_USB 1
#else
#define USB_STORAGE_HAVE_USB 0
#endif

typedef enum {
    UsbStorageStateInit,
    UsbStorageStateActive,
    UsbStorageStateError,
} UsbStorageState;

typedef struct {
    UsbStorageState state;
    const char* error_msg;
    FuriMutex* draw_mutex;
    FuriSemaphore* exit;
} UsbStorageCtx;

static void usb_storage_draw_callback(Canvas* canvas, void* context) {
    UsbStorageCtx* ctx = context;
    furi_mutex_acquire(ctx->draw_mutex, FuriWaitForever);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "USB-Storage");
    canvas_draw_line(canvas, 16, 13, 112, 13);

    canvas_set_font(canvas, FontSecondary);

    switch(ctx->state) {
    case UsbStorageStateInit:
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Mounting on PC ...");
        break;
    case UsbStorageStateActive:
        canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignCenter, "SD visible as USB drive");
        canvas_set_font(canvas, FontBatteryPercent);
        canvas_draw_str_aligned(
            canvas, 64, 36, AlignCenter, AlignCenter, "Eject from PC first,");
        canvas_draw_str_aligned(
            canvas, 64, 46, AlignCenter, AlignCenter, "then press Disconnect.");
        canvas_set_font(canvas, FontSecondary);
        elements_button_left(canvas, "Disconnect");
        break;
    case UsbStorageStateError:
        canvas_draw_str_aligned(
            canvas, 64, 28, AlignCenter, AlignCenter, ctx->error_msg ? ctx->error_msg : "Fehler");
        elements_button_left(canvas, "Back");
        break;
    }

    furi_mutex_release(ctx->draw_mutex);
}

static void usb_storage_input_callback(InputEvent* event, void* context) {
    UsbStorageCtx* ctx = context;
    if(event->type != InputTypeShort) return;
    if(event->key == InputKeyBack || event->key == InputKeyUp) {
        furi_semaphore_release(ctx->exit);
    }
}

#if USB_STORAGE_HAVE_USB
static bool usb_storage_enter(UsbStorageCtx* ctx, Storage* storage, bool* sd_was_mounted) {
    /* 1) Try to unmount on the storage-service side first. This closes any
     *    file handles other apps had open. Best-effort: an error here usually
     *    means the SD was never mounted, which is fine for our purposes. */
    *sd_was_mounted = (storage_sd_status(storage) == FSE_OK);
    if(*sd_was_mounted) {
        FS_Error err = storage_sd_unmount(storage);
        if(err != FSE_OK) {
            FURI_LOG_W(TAG, "storage_sd_unmount returned %d (continuing)", err);
        }
    }

    /* 2) Release FATFS in the HAL but keep sdmmc_card_t alive so MSC can
     *    read/write sectors directly. */
    if(!furi_hal_sd_release_fatfs()) {
        ctx->error_msg = "FATFS release failed";
        return false;
    }

    /* 3) Ensure the composite (HID + CDC + MSC) is installed. The qFlipper app
     *    may already have installed it; this call is a no-op if so. */
    if(!furi_hal_usb_composite_install(0, 0, NULL, NULL)) {
        ctx->error_msg = "USB composite install failed";
        return false;
    }

    /* 4) Flip MSC active. tud_msc_test_unit_ready_cb starts answering "ready"
     *    and the host re-reads capacity / partition table. */
    if(!furi_hal_usb_msc_start()) {
        ctx->error_msg = "MSC start failed";
        return false;
    }

    return true;
}

static void usb_storage_leave(Storage* storage, bool sd_was_mounted) {
    /* stop() flips s_active to false and arms the SCSI sense to
     * NOT_READY/MEDIUM_NOT_PRESENT. The host polls TEST UNIT READY every
     * ~1 sec; we wait long enough for at least one such poll so the host
     * sees the medium go away and unmounts the volume before we re-attach
     * the SD on the firmware side. Otherwise macOS keeps the kext mounted
     * and any later access either hangs or returns I/O errors. */
    furi_hal_usb_msc_stop();
    furi_delay_ms(1500);

    if(sd_was_mounted) {
        FS_Error err = storage_sd_mount(storage);
        if(err != FSE_OK) {
            FURI_LOG_E(TAG, "storage_sd_mount after MSC failed: %d", err);
        }
    }
}
#endif /* USB_STORAGE_HAVE_USB */

int32_t usb_storage_app(void* p) {
    UNUSED(p);

    UsbStorageCtx ctx = {
        .state = UsbStorageStateInit,
        .error_msg = NULL,
        .draw_mutex = furi_mutex_alloc(FuriMutexTypeNormal),
        .exit = furi_semaphore_alloc(1, 0),
    };

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, usb_storage_draw_callback, &ctx);
    view_port_input_callback_set(view_port, usb_storage_input_callback, &ctx);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

#if USB_STORAGE_HAVE_USB
    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool sd_was_mounted = false;

    view_port_update(view_port);

    if(usb_storage_enter(&ctx, storage, &sd_was_mounted)) {
        furi_mutex_acquire(ctx.draw_mutex, FuriWaitForever);
        ctx.state = UsbStorageStateActive;
        furi_mutex_release(ctx.draw_mutex);
        view_port_update(view_port);

        furi_semaphore_acquire(ctx.exit, FuriWaitForever);

        usb_storage_leave(storage, sd_was_mounted);
    } else {
        furi_mutex_acquire(ctx.draw_mutex, FuriWaitForever);
        ctx.state = UsbStorageStateError;
        furi_mutex_release(ctx.draw_mutex);
        view_port_update(view_port);
        furi_semaphore_acquire(ctx.exit, FuriWaitForever);
    }

    furi_record_close(RECORD_STORAGE);
#else
    ctx.state = UsbStorageStateError;
    ctx.error_msg = "USB-Storage nicht\nverfügbar auf\ndiesem Board";
    view_port_update(view_port);
    furi_semaphore_acquire(ctx.exit, FuriWaitForever);
#endif

    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(view_port);
    furi_semaphore_free(ctx.exit);
    furi_mutex_free(ctx.draw_mutex);

    return 0;
}
