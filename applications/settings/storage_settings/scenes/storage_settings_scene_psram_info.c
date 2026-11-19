#include "../storage_settings.h"
#include <furi_hal_psram.h>

// Only reachable on boards with PSRAM: the start menu lists "About PSRAM" only
// when furi_hal_psram_is_available().

static void
    storage_settings_scene_psram_info_dialog_callback(DialogExResult result, void* context) {
    StorageSettings* app = context;

    view_dispatcher_send_custom_event(app->view_dispatcher, result);
}

void storage_settings_scene_psram_info_on_enter(void* context) {
    StorageSettings* app = context;
    DialogEx* dialog_ex = app->dialog_ex;

    dialog_ex_set_context(dialog_ex, app);
    dialog_ex_set_result_callback(dialog_ex, storage_settings_scene_psram_info_dialog_callback);

    FuriHalPsramInfo info;
    if(!furi_hal_psram_get_info(&info) || info.total == 0) {
        dialog_ex_set_header(dialog_ex, "PSRAM Not Available", 64, 10, AlignCenter, AlignCenter);
        dialog_ex_set_text(
            dialog_ex, "This board has\nno PSRAM.", 64, 32, AlignCenter, AlignCenter);
    } else {
        // Same layout as "About Internal Storage": label lines from the top left.
        furi_string_printf(app->text_string, "Type: %s\n", info.type);
        if(info.speed_mhz) {
            furi_string_cat_printf(app->text_string, "Speed: %lu MHz\n", info.speed_mhz);
        }
        furi_string_cat_printf(
            app->text_string,
            "Total: %lu KiB\nFree: %lu KiB (%lu%%)",
            (uint32_t)(info.total / 1024),
            (uint32_t)(info.free / 1024),
            (uint32_t)(((uint64_t)info.free * 100) / info.total));

        dialog_ex_set_text(
            dialog_ex, furi_string_get_cstr(app->text_string), 4, 4, AlignLeft, AlignTop);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, StorageSettingsViewDialogEx);
}

bool storage_settings_scene_psram_info_on_event(void* context, SceneManagerEvent event) {
    StorageSettings* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case DialogExResultLeft:
            if(app->from_favorites) {
                scene_manager_stop(app->scene_manager);
                view_dispatcher_stop(app->view_dispatcher);
                return true;
            } else {
                consumed = scene_manager_previous_scene(app->scene_manager);
            }
            break;
        }
    }
    return consumed;
}

void storage_settings_scene_psram_info_on_exit(void* context) {
    StorageSettings* app = context;
    DialogEx* dialog_ex = app->dialog_ex;

    dialog_ex_reset(dialog_ex);

    furi_string_reset(app->text_string);
}
