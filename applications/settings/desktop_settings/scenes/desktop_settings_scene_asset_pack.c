#include <storage/storage.h>
#include <storage/filesystem_api_defines.h>
#include <string.h>

#include <desktop/desktop_settings.h>

#include "../desktop_settings_app.h"
#include "desktop_settings_scene.h"

static void desktop_settings_scene_asset_pack_cb(void* context, uint32_t index) {
    DesktopSettingsApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void desktop_settings_scene_asset_pack_on_enter(void* context) {
    DesktopSettingsApp* app = context;
    Submenu* submenu = app->submenu;
    submenu_reset(submenu);
    app->asset_pack_pick_count = 0;

    submenu_add_item(
        submenu, "Default (/ext/dolphin)", 0, desktop_settings_scene_asset_pack_cb, app);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);

    if(storage_dir_open(dir, EXT_PATH("asset_packs"))) {
        FileInfo finfo;
        char name[256];
        while(storage_dir_read(dir, &finfo, name, sizeof(name) - 1)) {
            if(!file_info_is_dir(&finfo)) {
                continue;
            }
            if(name[0] == '.') {
                continue;
            }
            size_t len = strlen(name);
            if(len == 0 || len >= DESKTOP_ASSET_PACK_NAME_LEN) {
                continue;
            }

            uint32_t menu_index = (uint32_t)app->asset_pack_pick_count + 1;
            if(menu_index >= DESKTOP_ASSET_PACK_MENU_MAX) {
                break;
            }

            strlcpy(
                app->asset_pack_pick_names[app->asset_pack_pick_count],
                name,
                DESKTOP_ASSET_PACK_NAME_LEN);
            submenu_add_item(
                submenu,
                app->asset_pack_pick_names[app->asset_pack_pick_count],
                menu_index,
                desktop_settings_scene_asset_pack_cb,
                app);
            app->asset_pack_pick_count++;
        }
        storage_dir_close(dir);
    }

    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);

    submenu_set_header(submenu, "Idle animation pack");
    view_dispatcher_switch_to_view(app->view_dispatcher, DesktopSettingsAppViewMenu);
}

bool desktop_settings_scene_asset_pack_on_event(void* context, SceneManagerEvent event) {
    DesktopSettingsApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        uint32_t idx = event.event;
        if(idx == 0) {
            app->settings.asset_pack[0] = '\0';
        } else if(idx > 0 && (idx - 1) < app->asset_pack_pick_count) {
            strlcpy(
                app->settings.asset_pack,
                app->asset_pack_pick_names[idx - 1],
                DESKTOP_ASSET_PACK_NAME_LEN);
        }
        desktop_settings_save(&app->settings);
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }

    return consumed;
}

void desktop_settings_scene_asset_pack_on_exit(void* context) {
    DesktopSettingsApp* app = context;
    submenu_reset(app->submenu);
}
