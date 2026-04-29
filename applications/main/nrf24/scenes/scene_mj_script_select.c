#include "../nrf24_app.h"

#include <dialogs/dialogs.h>
#include <storage/storage.h>
#include <assets_icons.h>

/* Scripts are shared with BadUSB. Same folder, same filtering. */
#define MJ_SCRIPT_BASE_PATH      "/ext/badusb"
#define MJ_SCRIPT_EXTENSION      ".txt"

void nrf24_app_scene_mj_script_select_on_enter(void* context) {
    Nrf24App* app = context;

    DialogsFileBrowserOptions options;
    dialog_file_browser_set_basic_options(&options, MJ_SCRIPT_EXTENSION, &I_badusb_10px);
    options.base_path = MJ_SCRIPT_BASE_PATH;
    options.skip_assets = true;

    if(furi_string_empty(app->mj_script_path)) {
        furi_string_set_str(app->mj_script_path, MJ_SCRIPT_BASE_PATH);
    }

    bool ok = dialog_file_browser_show(
        app->dialogs, app->mj_script_path, app->mj_script_path, &options);

    if(ok) {
        if(app->mj_auto_mode) {
            scene_manager_next_scene(app->scene_manager, Nrf24AppSceneMjAuto);
        } else {
            scene_manager_next_scene(app->scene_manager, Nrf24AppSceneMjAttack);
        }
    } else {
        scene_manager_previous_scene(app->scene_manager);
    }
}

bool nrf24_app_scene_mj_script_select_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void nrf24_app_scene_mj_script_select_on_exit(void* context) {
    UNUSED(context);
}
