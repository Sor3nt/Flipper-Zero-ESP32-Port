/**
 * "Other OS" / multi-boot launcher.
 *
 * This board ships with two firmwares flashed side by side (see
 * partitions_multiboot.csv and 00_Skills/multi-boot.md):
 *   - ota_0: this ESP32 Flipper Zero port  (default boot target)
 *   - ota_1: the Bruce firmware            (https://github.com/BruceDevices/firmware)
 *
 * This app sits in the main menu as "Bruce". Selecting it asks for
 * confirmation, points the OTA boot slot at ota_1 and reboots. Bruce has the
 * mirror-image entry ("Flipper Zero") that points back at ota_0.
 *
 * The confirmation screen is a custom view (the T-Embed rotary encoder maps to
 * Up/Down): Up = Cancel, Down = Load.
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/elements.h>
#include <dialogs/dialogs.h>
#include <input/input.h>

#include <esp_ota_ops.h>
#include <esp_partition.h>

#define TAG "OtherOS"

// The firmware we want to jump to lives in the ota_1 slot.
#define OTHER_OS_TARGET_SUBTYPE ESP_PARTITION_SUBTYPE_APP_OTA_1
#define OTHER_OS_NAME           "Bruce"

typedef enum {
    OtherOsChoiceNone,
    OtherOsChoiceCancel,
    OtherOsChoiceLoad,
} OtherOsChoice;

typedef struct {
    OtherOsChoice choice;
    FuriSemaphore* done;
} OtherOsCtx;

static bool other_os_select_boot_partition(void) {
    const esp_partition_t* target =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, OTHER_OS_TARGET_SUBTYPE, NULL);
    if(target == NULL) {
        FURI_LOG_E(TAG, "no '%s' partition found - not a multi-boot image?", OTHER_OS_NAME);
        return false;
    }

    esp_err_t err = esp_ota_set_boot_partition(target);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return false;
    }

    FURI_LOG_I(TAG, "boot partition set to %s @ 0x%08lx", target->label, (unsigned long)target->address);
    return true;
}

static void other_os_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    canvas_clear(canvas);

    const char* title = "Switch Firmware";
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, title);
    uint16_t tw = canvas_string_width(canvas, title);
    canvas_draw_line(canvas, 64 - tw / 2, 13, 64 + tw / 2, 13);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas, 64, 32, AlignCenter, AlignCenter, "Reboot into " OTHER_OS_NAME " ?");

    elements_button_left(canvas, "Cancel");
    elements_button_right(canvas, "Load");
}

static void other_os_input_callback(InputEvent* event, void* context) {
    OtherOsCtx* ctx = context;
    if(event->type != InputTypeShort) return;

    switch(event->key) {
    case InputKeyDown:
        ctx->choice = OtherOsChoiceLoad;
        furi_semaphore_release(ctx->done);
        break;
    case InputKeyUp:
    case InputKeyBack:
        ctx->choice = OtherOsChoiceCancel;
        furi_semaphore_release(ctx->done);
        break;
    default:
        break;
    }
}

static OtherOsChoice other_os_confirm(void) {
    OtherOsCtx ctx = {
        .choice = OtherOsChoiceNone,
        .done = furi_semaphore_alloc(1, 0),
    };

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, other_os_draw_callback, &ctx);
    view_port_input_callback_set(view_port, other_os_input_callback, &ctx);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    furi_semaphore_acquire(ctx.done, FuriWaitForever);

    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(view_port);
    furi_semaphore_free(ctx.done);

    return ctx.choice;
}

static void other_os_show_error(DialogsApp* dialogs) {
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "Multi-boot error", 64, 0, AlignCenter, AlignTop);
    dialog_message_set_text(
        message,
        "No \"" OTHER_OS_NAME "\" partition.\nFirmware was not flashed\nin multi-boot mode.",
        64,
        32,
        AlignCenter,
        AlignCenter);
    dialog_message_set_buttons(message, NULL, "OK", NULL);
    dialog_message_show(dialogs, message);
    dialog_message_free(message);
}

int32_t other_os_app(void* p) {
    UNUSED(p);

    if(other_os_confirm() == OtherOsChoiceLoad) {
        if(other_os_select_boot_partition()) {
            FURI_LOG_I(TAG, "rebooting into %s", OTHER_OS_NAME);
            furi_delay_ms(100);
            furi_hal_power_reset();
            // not reached
            return 0;
        }
        DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
        other_os_show_error(dialogs);
        furi_record_close(RECORD_DIALOGS);
    }

    return 0;
}
