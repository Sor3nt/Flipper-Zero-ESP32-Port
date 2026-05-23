#include "../momentum_app.h"
#include "momentum_app_scene.h"
#include <momentum/momentum_settings.h>
#include <momentum/asset_packs.h>
#include <storage/storage.h>
#include <furi.h>

void momentum_scene_interface_graphics_pack_on_enter(void* context) {
    MomentumApp* app = context;
    Submenu* submenu = app->submenu;

    submenu_reset(submenu);
    submenu_add_item(submenu, "None (default)", 0, NULL, app);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = furi_string_alloc_set(ASSET_PACKS_PATH);
    File* dir = storage_file_alloc(storage);

    if(storage_dir_open(dir, furi_string_get_cstr(path))) {
        FileInfo info;
        char name[64];
        int idx = 1;
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            if(file_info_is_dir(&info)) {
                submenu_add_item(submenu, name, idx++, NULL, app);
            }
        }
        storage_dir_close(dir);
    }

    storage_file_free(dir);
    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);

    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumAppViewSubmenu);
}

bool momentum_scene_interface_graphics_pack_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void momentum_scene_interface_graphics_pack_on_exit(void* context) {
    UNUSED(context);
}
