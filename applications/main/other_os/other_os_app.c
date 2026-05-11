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
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <dialogs/dialogs.h>

#include <esp_ota_ops.h>
#include <esp_partition.h>

#define TAG "OtherOS"

// The firmware we want to jump to lives in the ota_1 slot.
#define OTHER_OS_TARGET_SUBTYPE ESP_PARTITION_SUBTYPE_APP_OTA_1
#define OTHER_OS_NAME           "Bruce"

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

static DialogMessageButton other_os_confirm(DialogsApp* dialogs) {
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "Switch to " OTHER_OS_NAME, 64, 0, AlignCenter, AlignTop);
    dialog_message_set_text(
        message,
        "Reboot into the " OTHER_OS_NAME " firmware?\n"
        "Use \"Flipper Zero\" in " OTHER_OS_NAME "\nto come back.",
        64,
        32,
        AlignCenter,
        AlignCenter);
    dialog_message_set_buttons(message, "Cancel", "Reboot", NULL);
    DialogMessageButton result = dialog_message_show(dialogs, message);
    dialog_message_free(message);
    return result;
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

    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);

    if(other_os_confirm(dialogs) == DialogMessageButtonCenter) {
        if(other_os_select_boot_partition()) {
            furi_record_close(RECORD_DIALOGS);
            FURI_LOG_I(TAG, "rebooting into %s", OTHER_OS_NAME);
            furi_delay_ms(100);
            furi_hal_power_reset();
            // not reached
            return 0;
        }
        other_os_show_error(dialogs);
    }

    furi_record_close(RECORD_DIALOGS);
    return 0;
}
