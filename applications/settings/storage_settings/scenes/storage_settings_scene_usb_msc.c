#include "../storage_settings.h"

#include <furi_hal_usb_msc.h>

#include <stdio.h>
#include <esp_err.h>

typedef enum {
    UsbMscDlgUnsupported = 0,
    UsbMscDlgNeedSd,
    UsbMscDlgConfirm,
    UsbMscDlgActive,
    UsbMscDlgError,
} UsbMscDlg;

static UsbMscDlg s_usb_msc_dlg;
static char s_usb_msc_err[48];

static void storage_settings_usb_msc_dialog_callback(DialogExResult result, void* context) {
    StorageSettings* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, result);
}

static void storage_settings_usb_msc_set_dialog(StorageSettings* app, UsbMscDlg dlg) {
    DialogEx* dialog_ex = app->dialog_ex;
    s_usb_msc_dlg = dlg;

    dialog_ex_reset(dialog_ex);
    dialog_ex_set_context(dialog_ex, app);
    dialog_ex_set_result_callback(dialog_ex, storage_settings_usb_msc_dialog_callback);

    switch(dlg) {
    case UsbMscDlgUnsupported:
        dialog_ex_set_header(dialog_ex, "USB Disk", 64, 10, AlignCenter, AlignCenter);
        dialog_ex_set_text(
            dialog_ex,
            "Not available in\nthis firmware build",
            64,
            32,
            AlignCenter,
            AlignCenter);
        dialog_ex_set_left_button_text(dialog_ex, "Back");
        dialog_ex_set_center_button_text(dialog_ex, NULL);
        dialog_ex_set_right_button_text(dialog_ex, NULL);
        break;
    case UsbMscDlgNeedSd:
        dialog_ex_set_header(dialog_ex, "USB Disk", 64, 10, AlignCenter, AlignCenter);
        dialog_ex_set_text(
            dialog_ex,
            "Mount the SD card\nin Storage first",
            64,
            32,
            AlignCenter,
            AlignCenter);
        dialog_ex_set_left_button_text(dialog_ex, "Back");
        dialog_ex_set_center_button_text(dialog_ex, NULL);
        dialog_ex_set_right_button_text(dialog_ex, NULL);
        break;
    case UsbMscDlgConfirm:
        dialog_ex_set_header(dialog_ex, "SD over USB?", 64, 10, AlignCenter, AlignCenter);
        dialog_ex_set_text(
            dialog_ex,
            "PC can manage SD.\nDevice cannot use\n/ext until you stop",
            64,
            28,
            AlignCenter,
            AlignCenter);
        dialog_ex_set_left_button_text(dialog_ex, "Cancel");
        dialog_ex_set_center_button_text(dialog_ex, NULL);
        dialog_ex_set_right_button_text(dialog_ex, "Start");
        break;
    case UsbMscDlgActive:
        dialog_ex_set_header(dialog_ex, "USB disk active", 64, 10, AlignCenter, AlignCenter);
        dialog_ex_set_text(
            dialog_ex,
            "Eject/safely remove\non PC, then tap Stop",
            64,
            32,
            AlignCenter,
            AlignCenter);
        dialog_ex_set_left_button_text(dialog_ex, NULL);
        dialog_ex_set_center_button_text(dialog_ex, NULL);
        dialog_ex_set_right_button_text(dialog_ex, "Stop");
        break;
    case UsbMscDlgError:
        dialog_ex_set_header(dialog_ex, "USB disk error", 64, 10, AlignCenter, AlignCenter);
        dialog_ex_set_text(dialog_ex, s_usb_msc_err, 64, 32, AlignCenter, AlignCenter);
        dialog_ex_set_left_button_text(dialog_ex, "Back");
        dialog_ex_set_center_button_text(dialog_ex, NULL);
        dialog_ex_set_right_button_text(dialog_ex, NULL);
        break;
    default:
        break;
    }
}

void storage_settings_scene_usb_msc_on_enter(void* context) {
    StorageSettings* app = context;

    s_usb_msc_err[0] = '\0';

    if(!furi_hal_usb_msc_is_supported()) {
        storage_settings_usb_msc_set_dialog(app, UsbMscDlgUnsupported);
    } else if(storage_sd_status(app->fs_api) != FSE_OK) {
        storage_settings_usb_msc_set_dialog(app, UsbMscDlgNeedSd);
    } else if(furi_hal_usb_msc_is_active()) {
        storage_settings_usb_msc_set_dialog(app, UsbMscDlgActive);
    } else {
        storage_settings_usb_msc_set_dialog(app, UsbMscDlgConfirm);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, StorageSettingsViewDialogEx);
}

bool storage_settings_scene_usb_msc_on_event(void* context, SceneManagerEvent event) {
    StorageSettings* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(s_usb_msc_dlg) {
        case UsbMscDlgUnsupported:
        case UsbMscDlgNeedSd:
        case UsbMscDlgError:
            if(event.event == DialogExResultLeft) {
                consumed = scene_manager_previous_scene(app->scene_manager);
            }
            break;
        case UsbMscDlgConfirm:
            if(event.event == DialogExResultLeft || event.event == DialogExResultCenter) {
                consumed = scene_manager_previous_scene(app->scene_manager);
            } else if(event.event == DialogExResultRight) {
                FS_Error sus = storage_sd_suspend_for_usb_msc(app->fs_api);
                if(sus != FSE_OK) {
                    snprintf(s_usb_msc_err, sizeof(s_usb_msc_err), "Could not unmount\nSD (files open?)");
                    storage_settings_usb_msc_set_dialog(app, UsbMscDlgError);
                    consumed = true;
                    break;
                }
                esp_err_t e = furi_hal_usb_msc_start();
                if(e != ESP_OK) {
                    storage_sd_resume_after_usb_msc(app->fs_api);
                    snprintf(
                        s_usb_msc_err,
                        sizeof(s_usb_msc_err),
                        "%s",
                        esp_err_to_name(e));
                    storage_settings_usb_msc_set_dialog(app, UsbMscDlgError);
                    consumed = true;
                    break;
                }
                storage_settings_usb_msc_set_dialog(app, UsbMscDlgActive);
                consumed = true;
            }
            break;
        case UsbMscDlgActive:
            if(event.event == DialogExResultRight) {
                furi_hal_usb_msc_stop();
                storage_sd_resume_after_usb_msc(app->fs_api);
                consumed = scene_manager_previous_scene(app->scene_manager);
            }
            break;
        default:
            break;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(s_usb_msc_dlg == UsbMscDlgActive) {
            consumed = true;
        }
    }

    return consumed;
}

void storage_settings_scene_usb_msc_on_exit(void* context) {
    StorageSettings* app = context;
    dialog_ex_reset(app->dialog_ex);
}
