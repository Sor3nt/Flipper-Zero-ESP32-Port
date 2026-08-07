/**
 * Flash-Mode scene: confirmation in front of the ROM download mode.
 *
 * Reached from the lock menu ("Flash Mode"). Right/Continue reboots the board
 * into the ROM download mode so esptool or a web flasher can talk to it over
 * USB — no BOOT/RST button needed, which is the whole point on the T-Embed
 * CC1101 where RST sits on the PCB under the back shell.
 *
 * The confirmation is not decoration: nothing on the device can leave download
 * mode again. Only a host running esptool (it clears the flag itself) or a
 * power cycle gets the firmware back. The user has to be told before, not
 * discover it in front of a black screen.
 *
 * Actions:
 *   - Right (Continue) -> furi_hal_power_enter_download_mode(), never returns
 *   - Left  (Cancel)   -> back to the lock menu
 *   - Back             -> same as Cancel
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/modules/dialog_ex.h>

#include "../desktop_i.h"
#include "desktop_scene.h"

#define TAG "DesktopFlashMode"

static void flash_mode_result_callback(DialogExResult result, void* context) {
    Desktop* desktop = context;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, (uint32_t)result);
}

void desktop_scene_flash_mode_on_enter(void* context) {
    Desktop* desktop = context;

    DialogEx* d = desktop->flash_mode_dialog;
    dialog_ex_reset(d);
    /* Vertical budget on the 128x64 canvas: the status bar owns the top
     * STATUS_BAR_Y_SHIFT px and elements_button_* own the bottom 12, leaving 39.
     * A FontPrimary header (~11) plus two FontSecondary lines (~11 each) is 33 —
     * three lines would be 44 and collide with both. Keep it at two. */
    dialog_ex_set_header(d, "Flash Mode", 64, STATUS_BAR_Y_SHIFT + 2, AlignCenter, AlignTop);
    dialog_ex_set_text(
        d,
        "Screen goes dark.\n"
        "A PC is needed to exit.",
        64,
        38,
        AlignCenter,
        AlignCenter);
    dialog_ex_set_left_button_text(d, "Cancel");
    dialog_ex_set_right_button_text(d, "Continue");
    dialog_ex_set_context(d, desktop);
    dialog_ex_set_result_callback(d, flash_mode_result_callback);

    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdFlashMode);
}

bool desktop_scene_flash_mode_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeBack) {
        /* Treat Back as Cancel — never fall through to the reboot. */
        scene_manager_previous_scene(desktop->scene_manager);
        return true;
    }

    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == DialogExResultRight) {
        FURI_LOG_I(TAG, "User confirmed, rebooting into download mode");
        furi_hal_power_enter_download_mode();
        /* Never reached. */
    } else if(event.event == DialogExResultLeft) {
        scene_manager_previous_scene(desktop->scene_manager);
        consumed = true;
    }

    return consumed;
}

void desktop_scene_flash_mode_on_exit(void* context) {
    Desktop* desktop = context;
    dialog_ex_reset(desktop->flash_mode_dialog);
}
