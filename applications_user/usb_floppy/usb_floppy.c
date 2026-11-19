#include "usb_floppy.h"

#include <furi_hal_usb.h>
#include <furi_hal_usb_tinyusb_composite.h>
#include <furi_hal_board_info.h>
#include <toolbox/path.h>
#include <gui/elements.h>

#define TAG "UsbFloppy"

// Stable lease owner token — its address identifies this app to the USB HAL.
static const char usb_floppy_owner[] = "USB Floppy";

// ---------------------------------------------------------------------------
// MSC backing callbacks
//
// These run on the TinyUSB task, NOT the GUI thread. They reach the image
// through the mounted filesystem (the SD stays mounted). Only the TinyUSB task
// touches app->file while MSC is active, so no extra lock is needed; the byte
// counters are cosmetic and may race harmlessly with the GUI thread.
// ---------------------------------------------------------------------------
static bool floppy_backing_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* out) {
    UsbFloppyApp* app = ctx;
    const uint32_t len = count * FLOPPY_BLOCK_SIZE;
    const uint64_t off = (uint64_t)lba * FLOPPY_BLOCK_SIZE;
    if(off + len > app->image_size) return false; // LBA out of range
    if(!storage_file_seek(app->file, (uint32_t)off, true)) return false;
    if(storage_file_read(app->file, out, len) != len) return false;
    app->bytes_read += len;
    app->last_activity_tick = furi_get_tick();
    return true;
}

static bool floppy_backing_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf) {
    UsbFloppyApp* app = ctx;
    if(app->write_protect) return false; // write-protected medium
    const uint32_t len = count * FLOPPY_BLOCK_SIZE;
    const uint64_t off = (uint64_t)lba * FLOPPY_BLOCK_SIZE;
    if(off + len > app->image_size) return false;
    if(!storage_file_seek(app->file, (uint32_t)off, true)) return false;
    if(storage_file_write(app->file, buf, len) != len) return false;
    // Flush each write so pulling the device (or pressing Back) does not lose
    // data the host already believes is committed.
    storage_file_sync(app->file);
    app->bytes_written += len;
    app->last_activity_tick = furi_get_tick();
    return true;
}

// ---------------------------------------------------------------------------
// USB ownership — acquire / release, mirroring the pc_monitor_usb lease flow
// (lease -> composite -> MSC -> reinit -> sleep veto), released in reverse.
// Every step records what it owns so a failed release keeps what is still ours
// and can be retried.
// ---------------------------------------------------------------------------
static bool usb_floppy_usb_release(UsbFloppyApp* app) {
    if(app->msc_active) {
        furi_hal_usb_msc_stop();
        app->msc_active = false;
        // Let the host see MEDIUM_NOT_PRESENT before the composite disappears.
        furi_delay_ms(USB_FLOPPY_EJECT_WAIT_MS);
    }

    // A lease that preempted another composite may find its own installed.
    if(app->usb_lease && furi_hal_usb_composite_is_installed()) {
        app->usb_composite = true;
    }
    if(app->usb_composite) {
        if(furi_hal_usb_composite_uninstall_for_owner(usb_floppy_owner)) {
            app->usb_composite = false;
        } else {
            FURI_LOG_E(TAG, "Could not uninstall the USB composite; keeping the lease");
        }
    }

    if(app->usb_lease && !app->usb_composite) {
        if(furi_hal_usb_lease_release(usb_floppy_owner, app->usb_previous)) {
            app->usb_lease = false;
        } else if(!furi_hal_usb_is_lease_owner(usb_floppy_owner)) {
            // The lease is gone (only resuming a preempted owner failed).
            app->usb_lease = false;
        } else {
            FURI_LOG_E(TAG, "Could not release the USB lease; USB stays reserved");
        }
    }

    // The clock veto goes last — USB may still need it while anything is ours.
    if(!app->usb_lease && !app->usb_composite && app->sleep_inhibited) {
        furi_hal_power_light_sleep_inhibit_exit();
        app->sleep_inhibited = false;
    }

    return !app->usb_lease && !app->usb_composite && !app->sleep_inhibited;
}

// Take the USB port and expose the image. On failure nothing stays half-owned
// and app->state is set to the matching error screen.
static bool usb_floppy_usb_acquire(UsbFloppyApp* app) {
    // Declared before any goto so no jump crosses its initialization.
    FuriHalUsbMscBacking backing = {0};

    if(!furi_hal_usb_lease_acquire(usb_floppy_owner, &app->usb_previous)) {
        // Locked (RPC), leased elsewhere, or a composite that cannot be preempted.
        app->state = UsbFloppyStateUsbBusy;
        return false;
    }
    app->usb_lease = true;

    if(!furi_hal_usb_composite_install_for_owner(usb_floppy_owner, 0, 0, NULL, NULL)) {
        app->error_reason = "USB composite install";
        FURI_LOG_E(TAG, "USB composite install failed");
        app->usb_composite = furi_hal_usb_composite_is_installed();
        app->state = UsbFloppyStateError;
        goto fail;
    }
    app->usb_composite = true;

    backing.ctx = app;
    backing.block_size = FLOPPY_BLOCK_SIZE;
    backing.block_count = app->block_count;
    backing.writable = !app->write_protect;
    backing.is_cdrom = false;
    memcpy(backing.vendor_id, "Flipper ", 8);
    memcpy(backing.product_id, "USB Floppy      ", 16);
    memcpy(backing.product_rev, "0100", 4);
    backing.read = floppy_backing_read;
    backing.write = floppy_backing_write;
    backing.eject = NULL;
    if(!furi_hal_usb_msc_start_backing(&backing)) {
        app->error_reason = "MSC start";
        FURI_LOG_E(TAG, "MSC backing start failed");
        app->state = UsbFloppyStateError;
        goto fail;
    }
    app->msc_active = true;

    furi_hal_usb_reinit_for_owner(usb_floppy_owner);

    furi_hal_power_light_sleep_inhibit_enter();
    app->sleep_inhibited = true;
    return true;

fail:
    (void)usb_floppy_usb_release(app);
    return false;
}

// ---------------------------------------------------------------------------
// Mount / unmount
// ---------------------------------------------------------------------------
static bool usb_floppy_mount(UsbFloppyApp* app) {
    app->error_reason = NULL;
    app->bytes_read = 0;
    app->bytes_written = 0;
    app->last_activity_tick = 0;

    app->file = storage_file_alloc(app->storage);
    FS_AccessMode mode = app->write_protect ? FSAM_READ : (FSAM_READ | FSAM_WRITE);
    if(!storage_file_open(
           app->file, furi_string_get_cstr(app->image_path), mode, FSOM_OPEN_EXISTING)) {
        FURI_LOG_E(TAG, "Cannot open image");
        app->error_reason = "Cannot open file";
        storage_file_free(app->file);
        app->file = NULL;
        app->state = UsbFloppyStateError;
        return false;
    }

    if(!usb_floppy_usb_acquire(app)) {
        // acquire set the error state and released whatever it took
        storage_file_close(app->file);
        storage_file_free(app->file);
        app->file = NULL;
        return false;
    }

    app->state = UsbFloppyStateWork;
    notification_message(app->notifications, &sequence_display_backlight_on);
    return true;
}

static void usb_floppy_unmount(UsbFloppyApp* app) {
    // Release USB first so no backing callback runs while we close the file.
    for(uint8_t i = 0; i < USB_FLOPPY_RELEASE_TRIES; i++) {
        if(usb_floppy_usb_release(app)) break;
        furi_delay_ms(USB_FLOPPY_RELEASE_RETRY_MS);
    }
    if(app->usb_lease || app->usb_composite) {
        FURI_LOG_E(TAG, "USB could not be released; it stays reserved until reboot");
    }
    if(app->file) {
        storage_file_close(app->file);
        storage_file_free(app->file);
        app->file = NULL;
    }
}

// True while the host has the volume mounted and locked (SCSI PREVENT MEDIUM
// REMOVAL). A host that never sends PREVENT (Windows "quick removal") reads as
// unlocked, so it never blocks Back.
static bool usb_floppy_host_holds_medium(void) {
    return furi_hal_usb_msc_is_active() && furi_hal_usb_msc_is_removal_locked();
}

// ---------------------------------------------------------------------------
// Image selection
// ---------------------------------------------------------------------------
static bool usb_floppy_select_image(UsbFloppyApp* app) {
    FuriString* result = furi_string_alloc();
    furi_string_set(result, app->image_path);

    DialogsFileBrowserOptions options;
    dialog_file_browser_set_basic_options(&options, USB_FLOPPY_IMAGE_EXT, NULL);
    options.base_path = USB_FLOPPY_APP_FOLDER;
    options.hide_ext = false;

    bool picked = dialog_file_browser_show(app->dialogs, result, app->image_path, &options);
    if(picked) furi_string_set(app->image_path, result);
    furi_string_free(result);
    if(!picked) return false;

    FileInfo info;
    if(storage_common_stat(app->storage, furi_string_get_cstr(app->image_path), &info) != FSE_OK ||
       (info.flags & FSF_DIRECTORY)) {
        app->state = UsbFloppyStateBadImage;
        return false;
    }
    if(info.size == FLOPPY_720K_SIZE) {
        app->block_count = FLOPPY_720K_BLOCKS;
    } else if(info.size == FLOPPY_144M_SIZE) {
        app->block_count = FLOPPY_144M_BLOCKS;
    } else {
        app->state = UsbFloppyStateBadImage;
        return false;
    }
    app->image_size = (uint32_t)info.size;

    FuriString* name = furi_string_alloc();
    path_extract_filename(app->image_path, name, false);
    strncpy(app->image_name, furi_string_get_cstr(name), sizeof(app->image_name) - 1);
    app->image_name[sizeof(app->image_name) - 1] = '\0';
    furi_string_free(name);
    return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void draw_header(Canvas* canvas, const char* title) {
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 13);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, title);
    canvas_set_color(canvas, ColorBlack);
}

static void draw_hint(Canvas* canvas, const char* left, const char* center, const char* right) {
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 53, 128, 11);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontSecondary);
    if(left) canvas_draw_str_aligned(canvas, 4, 54, AlignLeft, AlignTop, left);
    if(center) canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignTop, center);
    if(right) canvas_draw_str_aligned(canvas, 124, 54, AlignRight, AlignTop, right);
    canvas_set_color(canvas, ColorBlack);
}

static const char* geometry_label(uint32_t block_count) {
    if(block_count == FLOPPY_720K_BLOCKS) return "720K";
    if(block_count == FLOPPY_144M_BLOCKS) return "1.44M";
    return "?";
}

static void draw_select(Canvas* canvas, UsbFloppyApp* app) {
    draw_header(canvas, "USB Floppy");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 18, AlignCenter, AlignTop, "Pick a 720K / 1.44M");
    canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignTop, "disk image (.img)");

    char wp[24];
    snprintf(wp, sizeof(wp), "Write protect: %s", app->write_protect ? "ON" : "off");
    canvas_draw_str_aligned(canvas, 64, 41, AlignCenter, AlignTop, wp);

    // PORT: on the encoder any wheel turn toggles write-protect (handle_select).
    draw_hint(
        canvas,
        "Back=exit",
        furi_hal_board_get_input_layout() == FuriHalBoardInputLayoutEncoder ? "turn=WP" : "<> WP",
        "OK=open");
}

static void draw_bad_image(Canvas* canvas, UsbFloppyApp* app) {
    UNUSED(app);
    draw_header(canvas, "USB Floppy");
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 18, AlignCenter, AlignTop, "Not a floppy image");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignTop, "Need exactly 737280 B");
    canvas_draw_str_aligned(canvas, 64, 41, AlignCenter, AlignTop, "(720K) or 1474560 B (1.44M)");
    draw_hint(canvas, "Back", NULL, "OK");
}

static void draw_usb_busy(Canvas* canvas, UsbFloppyApp* app) {
    UNUSED(app);
    draw_header(canvas, "USB Floppy");
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignTop, "USB is busy");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignTop, "Close USB storage /");
    canvas_draw_str_aligned(canvas, 64, 43, AlignCenter, AlignTop, "qFlipper, then retry");
    draw_hint(canvas, "Back", NULL, "OK");
}

static void draw_error(Canvas* canvas, UsbFloppyApp* app) {
    draw_header(canvas, "USB Floppy");
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignTop, "Mount failed");
    // PORT: say which step failed (lease/composite/MSC/file) instead of a bare message.
    if(app->error_reason) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignTop, app->error_reason);
    }
    draw_hint(canvas, "Back", NULL, "OK");
}

static void draw_work(Canvas* canvas, UsbFloppyApp* app) {
    draw_header(canvas, "USB Floppy");
    canvas_set_font(canvas, FontSecondary);

    // Spinning activity dot when there was recent IO.
    bool active = (furi_get_tick() - app->last_activity_tick) < furi_ms_to_ticks(400) &&
                  app->last_activity_tick != 0;
    if(active) canvas_draw_disc(canvas, 6, 6, 3);

    canvas_draw_str_aligned(canvas, 64, 16, AlignCenter, AlignTop, app->image_name);

    char line[40];
    snprintf(
        line,
        sizeof(line),
        "%s  %s",
        geometry_label(app->block_count),
        app->write_protect ? "read-only" : "read/write");
    canvas_draw_str_aligned(canvas, 64, 27, AlignCenter, AlignTop, line);

    snprintf(
        line,
        sizeof(line),
        "R %luK  W %luK",
        (unsigned long)(app->bytes_read / 1024),
        (unsigned long)(app->bytes_written / 1024));
    canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignTop, line);

    draw_hint(canvas, "Back=stop", NULL, NULL);
}

static void draw_eject_warn(Canvas* canvas, UsbFloppyApp* app) {
    UNUSED(app);
    draw_header(canvas, "Eject first");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 18, AlignCenter, AlignTop, "Host still has the disk");
    canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignTop, "mounted. Eject on the PC");
    canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignTop, "to avoid data loss.");
    draw_hint(canvas, "Hold Back=force", NULL, NULL);
}

static void app_draw_callback(Canvas* canvas, void* ctx) {
    UsbFloppyApp* app = ctx;
    if(furi_mutex_acquire(app->mutex, FuriWaitForever) != FuriStatusOk) return;
    canvas_clear(canvas);
    switch(app->state) {
    case UsbFloppyStateSelect:
        draw_select(canvas, app);
        break;
    case UsbFloppyStateBadImage:
        draw_bad_image(canvas, app);
        break;
    case UsbFloppyStateUsbBusy:
        draw_usb_busy(canvas, app);
        break;
    case UsbFloppyStateError:
        draw_error(canvas, app);
        break;
    case UsbFloppyStateWork:
        draw_work(canvas, app);
        break;
    case UsbFloppyStateEjectWarn:
        draw_eject_warn(canvas, app);
        break;
    }
    furi_mutex_release(app->mutex);
}

static void app_input_callback(InputEvent* input_event, void* ctx) {
    UsbFloppyApp* app = ctx;
    // Stock stream on every board; only presses/clicks matter here. Never block
    // the GUI thread behind a busy main loop.
    furi_message_queue_put(app->event_queue, input_event, 0);
}

// ---------------------------------------------------------------------------
// Input handling (runs on the main loop thread under the mutex)
// ---------------------------------------------------------------------------
static void handle_select(UsbFloppyApp* app, InputEvent* ev) {
    if(ev->type != InputTypeShort) return;
    switch(ev->key) {
    case InputKeyLeft:
    case InputKeyRight:
    case InputKeyUp:
    case InputKeyDown:
        app->write_protect = !app->write_protect;
        break;
    case InputKeyOk: {
        // The browser blocks; release the mutex so a repaint cannot deadlock.
        furi_mutex_release(app->mutex);
        bool ok = usb_floppy_select_image(app);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(ok) {
            furi_mutex_release(app->mutex);
            usb_floppy_mount(app); // sets Work or an error state
            furi_mutex_acquire(app->mutex, FuriWaitForever);
        }
        break;
    }
    case InputKeyBack:
        app->running = false;
        break;
    default:
        break;
    }
}

static void handle_message(UsbFloppyApp* app, InputEvent* ev) {
    if(ev->type != InputTypeShort) return;
    if(ev->key == InputKeyBack || ev->key == InputKeyOk) {
        app->state = UsbFloppyStateSelect;
    }
}

static void handle_work(UsbFloppyApp* app, InputEvent* ev) {
    if(ev->type == InputTypeShort && ev->key == InputKeyBack) {
        if(usb_floppy_host_holds_medium()) {
            app->state = UsbFloppyStateEjectWarn;
            return;
        }
        furi_mutex_release(app->mutex);
        usb_floppy_unmount(app);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->state = UsbFloppyStateSelect;
    }
}

static void handle_eject_warn(UsbFloppyApp* app, InputEvent* ev) {
    // Long Back forces the exit even while the host holds the medium.
    if(ev->key == InputKeyBack && ev->type == InputTypeLong) {
        FURI_LOG_W(TAG, "Forced exit while host still holds the medium");
        furi_mutex_release(app->mutex);
        usb_floppy_unmount(app);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->state = UsbFloppyStateSelect;
    } else if(ev->key == InputKeyBack && ev->type == InputTypeShort) {
        // Changed their mind: back to the work screen.
        app->state = UsbFloppyStateWork;
    }
}

// ---------------------------------------------------------------------------
// Board gate (supported boards only) + app entry
// ---------------------------------------------------------------------------
static bool usb_floppy_board_supported(void) {
    if(furi_hal_board_supports_ported_apps()) return true;
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "Board not supported", 64, 2, AlignCenter, AlignTop);
    dialog_message_set_text(
        message,
        "Runs on T-Embed, Paradoxt,\nThorChimera and Thormini\nonly.",
        64,
        34,
        AlignCenter,
        AlignCenter);
    dialog_message_set_buttons(message, NULL, "OK", NULL);
    dialog_message_show(dialogs, message);
    dialog_message_free(message);
    furi_record_close(RECORD_DIALOGS);
    return false;
}

int32_t usb_floppy_app(void* p) {
    UNUSED(p);
    if(!usb_floppy_board_supported()) return 0;

    UsbFloppyApp* app = malloc(sizeof(UsbFloppyApp));
    furi_check(app != NULL);
    memset(app, 0, sizeof(UsbFloppyApp));

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_check(app->mutex != NULL);
    app->event_queue = furi_message_queue_alloc(16, sizeof(InputEvent));
    app->image_path = furi_string_alloc_set(USB_FLOPPY_APP_FOLDER);
    app->state = UsbFloppyStateSelect;
    app->write_protect = false;
    app->running = true;

    app->storage = furi_record_open(RECORD_STORAGE);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    storage_common_mkdir(app->storage, USB_FLOPPY_APP_FOLDER); // ensure folder exists

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, app_draw_callback, app);
    view_port_input_callback_set(app->view_port, app_input_callback, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    InputEvent event;
    while(app->running) {
        // 200 ms tick: repaint the work screen stats and auto-clear the eject
        // warning once the host releases the medium.
        FuriStatus status = furi_message_queue_get(app->event_queue, &event, 200);

        if(furi_mutex_acquire(app->mutex, FuriWaitForever) != FuriStatusOk) continue;

        if(status == FuriStatusOk) {
            switch(app->state) {
            case UsbFloppyStateSelect:
                handle_select(app, &event);
                break;
            case UsbFloppyStateBadImage:
            case UsbFloppyStateUsbBusy:
            case UsbFloppyStateError:
                handle_message(app, &event);
                break;
            case UsbFloppyStateWork:
                handle_work(app, &event);
                break;
            case UsbFloppyStateEjectWarn:
                handle_eject_warn(app, &event);
                break;
            }
        } else {
            // Tick: the host may have ejected while the warning is up.
            if(app->state == UsbFloppyStateEjectWarn && !usb_floppy_host_holds_medium()) {
                furi_mutex_release(app->mutex);
                usb_floppy_unmount(app);
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->state = UsbFloppyStateSelect;
            }
        }

        furi_mutex_release(app->mutex);
        view_port_update(app->view_port);
    }

    // Cleanup — always give USB back even if the user quit from Select.
    view_port_enabled_set(app->view_port, false);
    gui_remove_view_port(app->gui, app->view_port);
    usb_floppy_unmount(app);

    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);

    furi_string_free(app->image_path);
    furi_message_queue_free(app->event_queue);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
