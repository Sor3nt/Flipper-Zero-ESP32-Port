#include "mass_storage.h"

#include <furi_hal_usb.h>
#include <furi_hal_usb_tinyusb_composite.h>
#include <furi_hal_board_info.h>
#include <toolbox/path.h>

#define TAG "MassStorage"

static const char mass_usb_owner[] = "Mass Storage";

// Preset sizes for "New image". Kept modest: FatFs has no sparse files, so
// creating an image allocates that much SD space (and takes a while for the
// big ones). Make larger images on a PC.
static const uint32_t mass_sizes[] = {
    1474560u, // 1.44M (floppy)
    2u << 20,
    4u << 20,
    8u << 20,
    16u << 20,
    32u << 20,
    64u << 20,
    128u << 20,
    256u << 20,
};
static const char* const mass_size_names[] =
    {"1.44M", "2M", "4M", "8M", "16M", "32M", "64M", "128M", "256M"};
#define MASS_SIZE_COUNT (sizeof(mass_sizes) / sizeof(mass_sizes[0]))
#define MASS_SIZE_DEFAULT 4 // 16M

// ---------------------------------------------------------------------------
// MSC backing callbacks (TinyUSB task) — writable 512-byte blocks.
// ---------------------------------------------------------------------------
static bool mass_backing_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* out) {
    MassStorageApp* app = ctx;
    const uint32_t len = count * MASS_BLOCK_SIZE;
    const uint64_t off = (uint64_t)lba * MASS_BLOCK_SIZE;
    if(off + len > app->image_size) return false;
    if(!storage_file_seek(app->file, (uint32_t)off, true)) return false;
    if(storage_file_read(app->file, out, len) != len) return false;
    app->bytes_read += len;
    app->last_activity_tick = furi_get_tick();
    return true;
}

static bool mass_backing_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf) {
    MassStorageApp* app = ctx;
    const uint32_t len = count * MASS_BLOCK_SIZE;
    const uint64_t off = (uint64_t)lba * MASS_BLOCK_SIZE;
    if(off + len > app->image_size) return false;
    if(!storage_file_seek(app->file, (uint32_t)off, true)) return false;
    if(storage_file_write(app->file, buf, len) != len) return false;
    storage_file_sync(app->file);
    app->bytes_written += len;
    app->last_activity_tick = furi_get_tick();
    return true;
}

// ---------------------------------------------------------------------------
// USB ownership — identical lease flow to usb_floppy.
// ---------------------------------------------------------------------------
static bool mass_usb_release(MassStorageApp* app) {
    if(app->msc_active) {
        furi_hal_usb_msc_stop();
        app->msc_active = false;
        furi_delay_ms(MASS_EJECT_WAIT_MS);
    }
    if(app->usb_lease && furi_hal_usb_composite_is_installed()) {
        app->usb_composite = true;
    }
    if(app->usb_composite) {
        if(furi_hal_usb_composite_uninstall_for_owner(mass_usb_owner)) {
            app->usb_composite = false;
        } else {
            FURI_LOG_E(TAG, "Could not uninstall the USB composite; keeping the lease");
        }
    }
    if(app->usb_lease && !app->usb_composite) {
        if(furi_hal_usb_lease_release(mass_usb_owner, app->usb_previous)) {
            app->usb_lease = false;
        } else if(!furi_hal_usb_is_lease_owner(mass_usb_owner)) {
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

static bool mass_usb_acquire(MassStorageApp* app) {
    FuriHalUsbMscBacking backing = {0};

    if(!furi_hal_usb_lease_acquire(mass_usb_owner, &app->usb_previous)) {
        app->state = MassStateUsbBusy;
        return false;
    }
    app->usb_lease = true;

    if(!furi_hal_usb_composite_install_for_owner(mass_usb_owner, 0, 0, NULL, NULL)) {
        app->error_reason = "USB composite install";
        FURI_LOG_E(TAG, "USB composite install failed");
        app->usb_composite = furi_hal_usb_composite_is_installed();
        app->state = MassStateError;
        goto fail;
    }
    app->usb_composite = true;

    backing.ctx = app;
    backing.block_size = MASS_BLOCK_SIZE;
    backing.block_count = app->block_count;
    backing.writable = true;
    backing.is_cdrom = false;
    memcpy(backing.vendor_id, "Flipper ", 8);
    memcpy(backing.product_id, "Mass Storage    ", 16);
    memcpy(backing.product_rev, "0001", 4);
    backing.read = mass_backing_read;
    backing.write = mass_backing_write;
    backing.eject = NULL;
    if(!furi_hal_usb_msc_start_backing(&backing)) {
        app->error_reason = "MSC start";
        FURI_LOG_E(TAG, "MSC backing start failed");
        app->state = MassStateError;
        goto fail;
    }
    app->msc_active = true;

    furi_hal_usb_reinit_for_owner(mass_usb_owner);
    furi_hal_power_light_sleep_inhibit_enter();
    app->sleep_inhibited = true;
    return true;

fail:
    (void)mass_usb_release(app);
    return false;
}

// ---------------------------------------------------------------------------
// Mount / unmount
// ---------------------------------------------------------------------------
static bool mass_mount(MassStorageApp* app) {
    app->error_reason = NULL;
    app->bytes_read = 0;
    app->bytes_written = 0;
    app->last_activity_tick = 0;

    app->file = storage_file_alloc(app->storage);
    if(!storage_file_open(
           app->file, furi_string_get_cstr(app->image_path), FSAM_READ | FSAM_WRITE,
           FSOM_OPEN_EXISTING)) {
        FURI_LOG_E(TAG, "Cannot open image");
        app->error_reason = "Cannot open file";
        storage_file_free(app->file);
        app->file = NULL;
        app->state = MassStateError;
        return false;
    }
    if(!mass_usb_acquire(app)) {
        storage_file_close(app->file);
        storage_file_free(app->file);
        app->file = NULL;
        return false;
    }
    app->state = MassStateWork;
    notification_message(app->notifications, &sequence_display_backlight_on);
    return true;
}

static void mass_unmount(MassStorageApp* app) {
    for(uint8_t i = 0; i < MASS_RELEASE_TRIES; i++) {
        if(mass_usb_release(app)) break;
        furi_delay_ms(MASS_RELEASE_RETRY_MS);
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

static bool mass_host_holds_medium(void) {
    return furi_hal_usb_msc_is_active() && furi_hal_usb_msc_is_removal_locked();
}

// ---------------------------------------------------------------------------
// Image selection / creation
// ---------------------------------------------------------------------------
static void mass_set_name(MassStorageApp* app) {
    FuriString* name = furi_string_alloc();
    path_extract_filename(app->image_path, name, false);
    strncpy(app->image_name, furi_string_get_cstr(name), sizeof(app->image_name) - 1);
    app->image_name[sizeof(app->image_name) - 1] = '\0';
    furi_string_free(name);
}

static bool mass_select_image(MassStorageApp* app) {
    FuriString* result = furi_string_alloc();
    furi_string_set(result, app->image_path);

    DialogsFileBrowserOptions options;
    dialog_file_browser_set_basic_options(&options, MASS_STORAGE_IMAGE_EXT, NULL);
    options.base_path = MASS_STORAGE_APP_FOLDER;
    options.hide_ext = false;

    bool picked = dialog_file_browser_show(app->dialogs, result, app->image_path, &options);
    if(picked) furi_string_set(app->image_path, result);
    furi_string_free(result);
    if(!picked) return false;

    FileInfo info;
    if(storage_common_stat(app->storage, furi_string_get_cstr(app->image_path), &info) != FSE_OK ||
       (info.flags & FSF_DIRECTORY)) {
        app->state = MassStateBadImage;
        return false;
    }
    if(info.size == 0 || (info.size % MASS_BLOCK_SIZE) != 0 || info.size > MASS_MAX_SIZE) {
        app->state = MassStateBadImage;
        return false;
    }
    app->image_size = (uint32_t)info.size;
    app->block_count = (uint32_t)(info.size / MASS_BLOCK_SIZE);
    mass_set_name(app);
    return true;
}

// Create a blank image of `size` bytes. Expands the file (FatFs allocates the
// clusters) and zeroes the first 4 KiB so the host sees an unformatted drive.
static bool mass_create_image(MassStorageApp* app, uint32_t size) {
    FuriString* path = furi_string_alloc();
    const char* sz = mass_size_names[app->new_size_index];
    FileInfo info;
    for(int n = 0;; n++) {
        if(n == 0) {
            furi_string_printf(
                path, "%snew_%s%s", MASS_STORAGE_APP_FOLDER, sz, MASS_STORAGE_IMAGE_EXT);
        } else {
            furi_string_printf(
                path, "%snew_%s_%d%s", MASS_STORAGE_APP_FOLDER, sz, n, MASS_STORAGE_IMAGE_EXT);
        }
        if(storage_common_stat(app->storage, furi_string_get_cstr(path), &info) != FSE_OK) break;
        if(n > 99) {
            furi_string_free(path);
            return false;
        }
    }

    File* f = storage_file_alloc(app->storage);
    bool ok = storage_file_open(
        f, furi_string_get_cstr(path), FSAM_READ | FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) ok = storage_file_seek(f, size, true); // expand to full size
    if(ok) ok = storage_file_seek(f, 0, true);
    if(ok) {
        uint8_t zero[512];
        memset(zero, 0, sizeof(zero));
        for(int i = 0; i < 8 && ok; i++) { // zero the first 4 KiB
            ok = storage_file_write(f, zero, sizeof(zero)) == sizeof(zero);
        }
    }
    storage_file_close(f);
    storage_file_free(f);

    if(ok) {
        furi_string_set(app->image_path, path);
        app->image_size = size;
        app->block_count = size / MASS_BLOCK_SIZE;
        mass_set_name(app);
    } else {
        // Clean up a half-made file.
        storage_simply_remove(app->storage, furi_string_get_cstr(path));
    }
    furi_string_free(path);
    return ok;
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

static void draw_select(Canvas* canvas, MassStorageApp* app) {
    draw_header(canvas, "Mass Storage");
    canvas_set_font(canvas, FontSecondary);

    // Two rows: Select image / New image.
    const char* rows[] = {"Select image", "New image"};
    for(uint8_t i = 0; i < MassSelectModeCount; i++) {
        uint8_t y = 17 + i * 12;
        if(i == app->select_mode) {
            canvas_draw_rbox(canvas, 6, y - 1, 116, 11, 2);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str_aligned(canvas, 10, y + 1, AlignLeft, AlignTop, rows[i]);
        if(i == MassSelectModeNew) {
            char sz[20];
            snprintf(sz, sizeof(sz), "< %s >", mass_size_names[app->new_size_index]);
            canvas_draw_str_aligned(canvas, 118, y + 1, AlignRight, AlignTop, sz);
        }
        if(i == app->select_mode) canvas_set_color(canvas, ColorBlack);
    }

    if(app->select_mode == MassSelectModeNew) {
        // PORT: on the encoder Left/Right (size) arrive as OK held + turn.
        draw_hint(
            canvas,
            "Back=exit",
            furi_hal_board_get_input_layout() == FuriHalBoardInputLayoutEncoder ? "OK+turn" : "<> size",
            "OK=create");
    } else {
        draw_hint(canvas, "Back=exit", NULL, "OK=open");
    }
}

static void draw_creating(Canvas* canvas, MassStorageApp* app) {
    draw_header(canvas, "Mass Storage");
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignTop, "Creating image...");
    canvas_set_font(canvas, FontSecondary);
    char sz[24];
    snprintf(sz, sizeof(sz), "%s - please wait", mass_size_names[app->new_size_index]);
    canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignTop, sz);
}

static void draw_bad_image(Canvas* canvas, MassStorageApp* app) {
    UNUSED(app);
    draw_header(canvas, "Mass Storage");
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignTop, "Bad image");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignTop, "Size must be a multiple");
    canvas_draw_str_aligned(canvas, 64, 43, AlignCenter, AlignTop, "of 512, under 4 GB");
    draw_hint(canvas, "Back", NULL, "OK");
}

static void draw_usb_busy(Canvas* canvas, MassStorageApp* app) {
    UNUSED(app);
    draw_header(canvas, "Mass Storage");
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignTop, "USB is busy");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignTop, "Close USB storage /");
    canvas_draw_str_aligned(canvas, 64, 43, AlignCenter, AlignTop, "qFlipper, then retry");
    draw_hint(canvas, "Back", NULL, "OK");
}

static void draw_error(Canvas* canvas, MassStorageApp* app) {
    draw_header(canvas, "Mass Storage");
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignTop, "Failed");
    // PORT: say which step failed (lease/composite/MSC/file) instead of a bare message.
    if(app->error_reason) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignTop, app->error_reason);
    }
    draw_hint(canvas, "Back", NULL, "OK");
}

static void draw_work(Canvas* canvas, MassStorageApp* app) {
    draw_header(canvas, "Mass Storage");
    canvas_set_font(canvas, FontSecondary);

    bool active = (furi_get_tick() - app->last_activity_tick) < furi_ms_to_ticks(400) &&
                  app->last_activity_tick != 0;
    if(active) canvas_draw_disc(canvas, 6, 6, 3);

    canvas_draw_str_aligned(canvas, 64, 16, AlignCenter, AlignTop, app->image_name);

    char line[40];
    uint32_t mib = app->image_size / (1024u * 1024u);
    uint32_t kib = app->image_size / 1024u;
    if(mib > 0) {
        snprintf(line, sizeof(line), "%lu MB  read/write", (unsigned long)mib);
    } else {
        snprintf(line, sizeof(line), "%lu KB  read/write", (unsigned long)kib);
    }
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

static void draw_eject_warn(Canvas* canvas, MassStorageApp* app) {
    UNUSED(app);
    draw_header(canvas, "Eject first");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignTop, "Host still has the drive");
    canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignTop, "mounted. Eject on the PC");
    canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignTop, "to avoid data loss.");
    draw_hint(canvas, "Hold Back=force", NULL, NULL);
}

static void app_draw_callback(Canvas* canvas, void* ctx) {
    MassStorageApp* app = ctx;
    if(furi_mutex_acquire(app->mutex, FuriWaitForever) != FuriStatusOk) return;
    canvas_clear(canvas);
    switch(app->state) {
    case MassStateSelect:
        draw_select(canvas, app);
        break;
    case MassStateCreating:
        draw_creating(canvas, app);
        break;
    case MassStateBadImage:
        draw_bad_image(canvas, app);
        break;
    case MassStateUsbBusy:
        draw_usb_busy(canvas, app);
        break;
    case MassStateError:
        draw_error(canvas, app);
        break;
    case MassStateWork:
        draw_work(canvas, app);
        break;
    case MassStateEjectWarn:
        draw_eject_warn(canvas, app);
        break;
    }
    furi_mutex_release(app->mutex);
}

static void app_input_callback(InputEvent* input_event, void* ctx) {
    MassStorageApp* app = ctx;
    furi_message_queue_put(app->event_queue, input_event, 0);
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------
static void handle_select(MassStorageApp* app, InputEvent* ev) {
    if(ev->type != InputTypeShort && ev->type != InputTypeRepeat) return;
    switch(ev->key) {
    case InputKeyUp:
        if(ev->type == InputTypeShort && app->select_mode > 0) app->select_mode--;
        break;
    case InputKeyDown:
        if(ev->type == InputTypeShort && app->select_mode < MassSelectModeCount - 1)
            app->select_mode++;
        break;
    case InputKeyLeft:
        if(app->select_mode == MassSelectModeNew && app->new_size_index > 0) app->new_size_index--;
        break;
    case InputKeyRight:
        if(app->select_mode == MassSelectModeNew && app->new_size_index < MASS_SIZE_COUNT - 1)
            app->new_size_index++;
        break;
    case InputKeyOk: {
        if(ev->type != InputTypeShort) break;
        if(app->select_mode == MassSelectModeOpen) {
            furi_mutex_release(app->mutex);
            bool ok = mass_select_image(app);
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            if(ok) {
                furi_mutex_release(app->mutex);
                mass_mount(app);
                furi_mutex_acquire(app->mutex, FuriWaitForever);
            }
        } else {
            uint32_t size = mass_sizes[app->new_size_index];
            app->error_reason = NULL;
            app->state = MassStateCreating;
            furi_mutex_release(app->mutex);
            view_port_update(app->view_port); // show "Creating..."
            bool ok = mass_create_image(app, size);
            if(ok) {
                mass_mount(app); // sets Work or an error state
            }
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            if(!ok) {
                app->state = MassStateError;
                app->error_reason = "Create image";
            }
        }
        break;
    }
    case InputKeyBack:
        if(ev->type == InputTypeShort) app->running = false;
        break;
    default:
        break;
    }
}

static void handle_message(MassStorageApp* app, InputEvent* ev) {
    if(ev->type != InputTypeShort) return;
    if(ev->key == InputKeyBack || ev->key == InputKeyOk) {
        app->state = MassStateSelect;
    }
}

static void handle_work(MassStorageApp* app, InputEvent* ev) {
    if(ev->type == InputTypeShort && ev->key == InputKeyBack) {
        if(mass_host_holds_medium()) {
            app->state = MassStateEjectWarn;
            return;
        }
        furi_mutex_release(app->mutex);
        mass_unmount(app);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->state = MassStateSelect;
    }
}

static void handle_eject_warn(MassStorageApp* app, InputEvent* ev) {
    if(ev->key == InputKeyBack && ev->type == InputTypeLong) {
        FURI_LOG_W(TAG, "Forced exit while host still holds the medium");
        furi_mutex_release(app->mutex);
        mass_unmount(app);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->state = MassStateSelect;
    } else if(ev->key == InputKeyBack && ev->type == InputTypeShort) {
        app->state = MassStateWork;
    }
}

// ---------------------------------------------------------------------------
// Board gate + entry
// ---------------------------------------------------------------------------
static bool mass_board_supported(void) {
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

int32_t mass_storage_app(void* p) {
    UNUSED(p);
    if(!mass_board_supported()) return 0;

    MassStorageApp* app = malloc(sizeof(MassStorageApp));
    furi_check(app != NULL);
    memset(app, 0, sizeof(MassStorageApp));

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_check(app->mutex != NULL);
    app->event_queue = furi_message_queue_alloc(16, sizeof(InputEvent));
    app->image_path = furi_string_alloc_set(MASS_STORAGE_APP_FOLDER);
    app->state = MassStateSelect;
    app->select_mode = MassSelectModeOpen;
    app->new_size_index = MASS_SIZE_DEFAULT;
    app->running = true;

    app->storage = furi_record_open(RECORD_STORAGE);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    storage_common_mkdir(app->storage, MASS_STORAGE_APP_FOLDER);

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
            case MassStateSelect:
                handle_select(app, &event);
                break;
            case MassStateCreating:
                break; // ignore input while creating
            case MassStateBadImage:
            case MassStateUsbBusy:
            case MassStateError:
                handle_message(app, &event);
                break;
            case MassStateWork:
                handle_work(app, &event);
                break;
            case MassStateEjectWarn:
                handle_eject_warn(app, &event);
                break;
            }
        } else {
            if(app->state == MassStateEjectWarn && !mass_host_holds_medium()) {
                furi_mutex_release(app->mutex);
                mass_unmount(app);
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->state = MassStateSelect;
            }
        }

        furi_mutex_release(app->mutex);
        view_port_update(app->view_port);
    }

    view_port_enabled_set(app->view_port, false);
    gui_remove_view_port(app->gui, app->view_port);
    mass_unmount(app);

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
