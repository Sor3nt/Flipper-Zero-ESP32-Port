#include "usb_cdrom.h"

#include <furi_hal_usb.h>
#include <furi_hal_usb_tinyusb_composite.h>
#include <furi_hal_board_info.h>
#include <toolbox/path.h>

#define TAG "UsbCdrom"

static const char usb_cdrom_owner[] = "USB CD-ROM";

// ---------------------------------------------------------------------------
// MSC backing callback — read-only, 2048-byte blocks, runs on the TinyUSB task.
// Only the TinyUSB task touches app->file while MSC is active.
// ---------------------------------------------------------------------------
static bool cdrom_backing_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* out) {
    UsbCdromApp* app = ctx;
    const uint32_t len = count * CD_BLOCK_SIZE;
    const uint64_t off = (uint64_t)lba * CD_BLOCK_SIZE;
    if(off + len > app->image_size) return false; // LBA out of range
    if(!storage_file_seek(app->file, (uint32_t)off, true)) return false;
    if(storage_file_read(app->file, out, len) != len) return false;
    app->bytes_read += len;
    app->last_activity_tick = furi_get_tick();
    return true;
}

// ---------------------------------------------------------------------------
// USB ownership — identical lease flow to usb_floppy (lease -> composite ->
// MSC backing -> reinit -> sleep veto), released in reverse with retries.
// ---------------------------------------------------------------------------
static bool usb_cdrom_usb_release(UsbCdromApp* app) {
    if(app->msc_active) {
        furi_hal_usb_msc_stop();
        app->msc_active = false;
        furi_delay_ms(USB_CDROM_EJECT_WAIT_MS);
    }
    if(app->usb_lease && furi_hal_usb_composite_is_installed()) {
        app->usb_composite = true;
    }
    if(app->usb_composite) {
        if(furi_hal_usb_composite_uninstall_for_owner(usb_cdrom_owner)) {
            app->usb_composite = false;
        } else {
            FURI_LOG_E(TAG, "Could not uninstall the USB composite; keeping the lease");
        }
    }
    if(app->usb_lease && !app->usb_composite) {
        if(furi_hal_usb_lease_release(usb_cdrom_owner, app->usb_previous)) {
            app->usb_lease = false;
        } else if(!furi_hal_usb_is_lease_owner(usb_cdrom_owner)) {
            app->usb_lease = false;
        } else {
            FURI_LOG_E(TAG, "Could not release the USB lease; USB stays reserved");
        }
    }
    if(!app->usb_lease && !app->usb_composite && app->sleep_inhibited) {
        furi_hal_power_light_sleep_inhibit_exit();
        app->sleep_inhibited = false;
    }
    return !app->usb_lease && !app->usb_composite && !app->sleep_inhibited;
}

static bool usb_cdrom_usb_acquire(UsbCdromApp* app) {
    FuriHalUsbMscBacking backing = {0};

    if(!furi_hal_usb_lease_acquire(usb_cdrom_owner, &app->usb_previous)) {
        app->state = UsbCdromStateUsbBusy;
        return false;
    }
    app->usb_lease = true;

    if(!furi_hal_usb_composite_install_for_owner(usb_cdrom_owner, 0, 0, NULL, NULL)) {
        app->error_reason = "USB composite install";
        FURI_LOG_E(TAG, "USB composite install failed");
        app->usb_composite = furi_hal_usb_composite_is_installed();
        app->state = UsbCdromStateError;
        goto fail;
    }
    app->usb_composite = true;

    backing.ctx = app;
    backing.block_size = CD_BLOCK_SIZE;
    backing.block_count = app->block_count;
    backing.writable = false; // optical media is read-only
    backing.is_cdrom = true;
    memcpy(backing.vendor_id, "Flipper ", 8);
    memcpy(backing.product_id, "USB CD-ROM      ", 16);
    memcpy(backing.product_rev, "0100", 4);
    backing.read = cdrom_backing_read;
    backing.write = NULL;
    backing.eject = NULL;
    if(!furi_hal_usb_msc_start_backing(&backing)) {
        app->error_reason = "MSC start";
        FURI_LOG_E(TAG, "MSC backing start failed");
        app->state = UsbCdromStateError;
        goto fail;
    }
    app->msc_active = true;

    furi_hal_usb_reinit_for_owner(usb_cdrom_owner);
    furi_hal_power_light_sleep_inhibit_enter();
    app->sleep_inhibited = true;
    return true;

fail:
    (void)usb_cdrom_usb_release(app);
    return false;
}

// ---------------------------------------------------------------------------
// Mount / unmount
// ---------------------------------------------------------------------------
static bool usb_cdrom_mount(UsbCdromApp* app) {
    app->error_reason = NULL;
    app->bytes_read = 0;
    app->last_activity_tick = 0;

    app->file = storage_file_alloc(app->storage);
    if(!storage_file_open(
           app->file, furi_string_get_cstr(app->image_path), FSAM_READ, FSOM_OPEN_EXISTING)) {
        FURI_LOG_E(TAG, "Cannot open ISO");
        app->error_reason = "Cannot open file";
        storage_file_free(app->file);
        app->file = NULL;
        app->state = UsbCdromStateError;
        return false;
    }
    if(!usb_cdrom_usb_acquire(app)) {
        storage_file_close(app->file);
        storage_file_free(app->file);
        app->file = NULL;
        return false;
    }
    app->state = UsbCdromStateWork;
    notification_message(app->notifications, &sequence_display_backlight_on);
    return true;
}

static void usb_cdrom_unmount(UsbCdromApp* app) {
    for(uint8_t i = 0; i < USB_CDROM_RELEASE_TRIES; i++) {
        if(usb_cdrom_usb_release(app)) break;
        furi_delay_ms(USB_CDROM_RELEASE_RETRY_MS);
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

static bool usb_cdrom_host_holds_medium(void) {
    return furi_hal_usb_msc_is_active() && furi_hal_usb_msc_is_removal_locked();
}

// ---------------------------------------------------------------------------
// ISO selection
// ---------------------------------------------------------------------------
static bool usb_cdrom_select_image(UsbCdromApp* app) {
    FuriString* result = furi_string_alloc();
    furi_string_set(result, app->image_path);

    DialogsFileBrowserOptions options;
    dialog_file_browser_set_basic_options(&options, USB_CDROM_IMAGE_EXT, NULL);
    options.base_path = USB_CDROM_APP_FOLDER;
    options.hide_ext = false;

    bool picked = dialog_file_browser_show(app->dialogs, result, app->image_path, &options);
    if(picked) furi_string_set(app->image_path, result);
    furi_string_free(result);
    if(!picked) return false;

    FileInfo info;
    if(storage_common_stat(app->storage, furi_string_get_cstr(app->image_path), &info) != FSE_OK ||
       (info.flags & FSF_DIRECTORY)) {
        app->state = UsbCdromStateBadImage;
        return false;
    }
    if(info.size == 0 || (info.size % CD_BLOCK_SIZE) != 0 || info.size > CD_MAX_SIZE) {
        app->state = UsbCdromStateBadImage;
        return false;
    }
    app->image_size = (uint32_t)info.size;
    app->block_count = (uint32_t)(info.size / CD_BLOCK_SIZE);

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

static void draw_select(Canvas* canvas, UsbCdromApp* app) {
    UNUSED(app);
    draw_header(canvas, "USB CD-ROM");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignTop, "Pick an .iso image");
    canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignTop, "to mount as a CD-ROM");
    draw_hint(canvas, "Back=exit", NULL, "OK=open");
}

static void draw_bad_image(Canvas* canvas, UsbCdromApp* app) {
    UNUSED(app);
    draw_header(canvas, "USB CD-ROM");
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignTop, "Not a valid ISO");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignTop, "Size must be a multiple");
    canvas_draw_str_aligned(canvas, 64, 43, AlignCenter, AlignTop, "of 2048, under 2 GB");
    draw_hint(canvas, "Back", NULL, "OK");
}

static void draw_usb_busy(Canvas* canvas, UsbCdromApp* app) {
    UNUSED(app);
    draw_header(canvas, "USB CD-ROM");
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignTop, "USB is busy");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignTop, "Close USB storage /");
    canvas_draw_str_aligned(canvas, 64, 43, AlignCenter, AlignTop, "qFlipper, then retry");
    draw_hint(canvas, "Back", NULL, "OK");
}

static void draw_error(Canvas* canvas, UsbCdromApp* app) {
    draw_header(canvas, "USB CD-ROM");
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignTop, "Mount failed");
    // PORT: say which step failed (lease/composite/MSC/file) instead of a bare message.
    if(app->error_reason) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignTop, app->error_reason);
    }
    draw_hint(canvas, "Back", NULL, "OK");
}

static void draw_work(Canvas* canvas, UsbCdromApp* app) {
    draw_header(canvas, "USB CD-ROM");
    canvas_set_font(canvas, FontSecondary);

    bool active = (furi_get_tick() - app->last_activity_tick) < furi_ms_to_ticks(400) &&
                  app->last_activity_tick != 0;
    if(active) canvas_draw_disc(canvas, 6, 6, 3);

    canvas_draw_str_aligned(canvas, 64, 16, AlignCenter, AlignTop, app->image_name);

    char line[40];
    uint32_t mib = app->image_size / (1024u * 1024u);
    snprintf(line, sizeof(line), "%lu MB  read-only", (unsigned long)mib);
    canvas_draw_str_aligned(canvas, 64, 27, AlignCenter, AlignTop, line);

    snprintf(line, sizeof(line), "Read %lu KB", (unsigned long)(app->bytes_read / 1024));
    canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignTop, line);

    draw_hint(canvas, "Back=stop", NULL, NULL);
}

static void draw_eject_warn(Canvas* canvas, UsbCdromApp* app) {
    UNUSED(app);
    draw_header(canvas, "Eject first");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignTop, "Host still has the disc");
    canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignTop, "mounted. Eject on the PC.");
    draw_hint(canvas, "Hold Back=force", NULL, NULL);
}

static void app_draw_callback(Canvas* canvas, void* ctx) {
    UsbCdromApp* app = ctx;
    if(furi_mutex_acquire(app->mutex, FuriWaitForever) != FuriStatusOk) return;
    canvas_clear(canvas);
    switch(app->state) {
    case UsbCdromStateSelect:
        draw_select(canvas, app);
        break;
    case UsbCdromStateBadImage:
        draw_bad_image(canvas, app);
        break;
    case UsbCdromStateUsbBusy:
        draw_usb_busy(canvas, app);
        break;
    case UsbCdromStateError:
        draw_error(canvas, app);
        break;
    case UsbCdromStateWork:
        draw_work(canvas, app);
        break;
    case UsbCdromStateEjectWarn:
        draw_eject_warn(canvas, app);
        break;
    }
    furi_mutex_release(app->mutex);
}

static void app_input_callback(InputEvent* input_event, void* ctx) {
    UsbCdromApp* app = ctx;
    furi_message_queue_put(app->event_queue, input_event, 0);
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------
static void handle_select(UsbCdromApp* app, InputEvent* ev) {
    if(ev->type != InputTypeShort) return;
    switch(ev->key) {
    case InputKeyOk: {
        furi_mutex_release(app->mutex);
        bool ok = usb_cdrom_select_image(app);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(ok) {
            furi_mutex_release(app->mutex);
            usb_cdrom_mount(app);
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

static void handle_message(UsbCdromApp* app, InputEvent* ev) {
    if(ev->type != InputTypeShort) return;
    if(ev->key == InputKeyBack || ev->key == InputKeyOk) {
        app->state = UsbCdromStateSelect;
    }
}

static void handle_work(UsbCdromApp* app, InputEvent* ev) {
    if(ev->type == InputTypeShort && ev->key == InputKeyBack) {
        if(usb_cdrom_host_holds_medium()) {
            app->state = UsbCdromStateEjectWarn;
            return;
        }
        furi_mutex_release(app->mutex);
        usb_cdrom_unmount(app);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->state = UsbCdromStateSelect;
    }
}

static void handle_eject_warn(UsbCdromApp* app, InputEvent* ev) {
    if(ev->key == InputKeyBack && ev->type == InputTypeLong) {
        FURI_LOG_W(TAG, "Forced exit while host still holds the medium");
        furi_mutex_release(app->mutex);
        usb_cdrom_unmount(app);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->state = UsbCdromStateSelect;
    } else if(ev->key == InputKeyBack && ev->type == InputTypeShort) {
        app->state = UsbCdromStateWork;
    }
}

// ---------------------------------------------------------------------------
// Board gate + entry
// ---------------------------------------------------------------------------
static bool usb_cdrom_board_supported(void) {
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

int32_t usb_cdrom_app(void* p) {
    UNUSED(p);
    if(!usb_cdrom_board_supported()) return 0;

    UsbCdromApp* app = malloc(sizeof(UsbCdromApp));
    furi_check(app != NULL);
    memset(app, 0, sizeof(UsbCdromApp));

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_check(app->mutex != NULL);
    app->event_queue = furi_message_queue_alloc(16, sizeof(InputEvent));
    app->image_path = furi_string_alloc_set(USB_CDROM_APP_FOLDER);
    app->state = UsbCdromStateSelect;
    app->running = true;

    app->storage = furi_record_open(RECORD_STORAGE);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    storage_common_mkdir(app->storage, USB_CDROM_APP_FOLDER);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, app_draw_callback, app);
    view_port_input_callback_set(app->view_port, app_input_callback, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    InputEvent event;
    while(app->running) {
        FuriStatus status = furi_message_queue_get(app->event_queue, &event, 200);
        if(furi_mutex_acquire(app->mutex, FuriWaitForever) != FuriStatusOk) continue;

        if(status == FuriStatusOk) {
            switch(app->state) {
            case UsbCdromStateSelect:
                handle_select(app, &event);
                break;
            case UsbCdromStateBadImage:
            case UsbCdromStateUsbBusy:
            case UsbCdromStateError:
                handle_message(app, &event);
                break;
            case UsbCdromStateWork:
                handle_work(app, &event);
                break;
            case UsbCdromStateEjectWarn:
                handle_eject_warn(app, &event);
                break;
            }
        } else {
            if(app->state == UsbCdromStateEjectWarn && !usb_cdrom_host_holds_medium()) {
                furi_mutex_release(app->mutex);
                usb_cdrom_unmount(app);
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->state = UsbCdromStateSelect;
            }
        }

        furi_mutex_release(app->mutex);
        view_port_update(app->view_port);
    }

    view_port_enabled_set(app->view_port, false);
    gui_remove_view_port(app->gui, app->view_port);
    usb_cdrom_unmount(app);

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
