/**
 * "Switch to Bruce" confirmation scene.
 *
 * This board ships with two firmwares flashed side by side (see
 * partitions_multiboot.csv and 00_Skills/multi-boot.md):
 *   - ota_0: this ESP32 Flipper Zero port  (default boot target)
 *   - ota_1: the Bruce firmware            (https://github.com/BruceDevices/firmware)
 *
 * Reached from the lock menu's "Switch to Bruce" entry. On confirm we point the
 * OTA boot slot at ota_1 and reboot; Bruce has the mirror-image entry that
 * points back at ota_0. (Formerly the standalone "other_os" main-menu app.)
 */

#include <furi.h>
#include <furi_hal.h>

#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "../desktop_i.h"
#include "../views/desktop_view_bruce_confirm.h"
#include "desktop_scene.h"

#define TAG "DesktopBruce"

/* The firmware we want to jump to lives in the ota_1 slot. */
#define BRUCE_TARGET_SUBTYPE ESP_PARTITION_SUBTYPE_APP_OTA_1

static bool desktop_bruce_select_boot_partition(void) {
    const esp_partition_t* target =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, BRUCE_TARGET_SUBTYPE, NULL);
    if(target == NULL) {
        FURI_LOG_E(TAG, "no 'Bruce' partition found - not a multi-boot image?");
        return false;
    }

    esp_err_t err = esp_ota_set_boot_partition(target);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return false;
    }

    FURI_LOG_I(
        TAG,
        "boot partition set to %s @ 0x%08lx",
        target->label,
        (unsigned long)target->address);
    return true;
}

void desktop_scene_bruce_confirm_callback(DesktopEvent event, void* context) {
    Desktop* desktop = (Desktop*)context;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, event);
}

void desktop_scene_bruce_confirm_on_enter(void* context) {
    Desktop* desktop = (Desktop*)context;

    desktop_bruce_confirm_set_callback(
        desktop->bruce_confirm_view, desktop_scene_bruce_confirm_callback, desktop);
    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdBruceConfirm);
}

bool desktop_scene_bruce_confirm_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = (Desktop*)context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case DesktopBruceEventLoad:
            if(desktop_bruce_select_boot_partition()) {
                FURI_LOG_I(TAG, "rebooting into Bruce");
                furi_delay_ms(100);
                furi_hal_power_reset();
                /* not reached */
            }
            /* No partition (not flashed multi-boot): just go back. */
            scene_manager_search_and_switch_to_previous_scene(
                desktop->scene_manager, DesktopSceneMain);
            consumed = true;
            break;
        case DesktopBruceEventCancel:
            scene_manager_search_and_switch_to_previous_scene(
                desktop->scene_manager, DesktopSceneMain);
            consumed = true;
            break;
        default:
            break;
        }
    }

    return consumed;
}

void desktop_scene_bruce_confirm_on_exit(void* context) {
    UNUSED(context);
}
