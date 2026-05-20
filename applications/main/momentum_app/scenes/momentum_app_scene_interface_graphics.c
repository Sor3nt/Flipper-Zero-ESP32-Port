#include "../momentum_app.h"
#include "momentum_app_scene.h"
#include <momentum/momentum_settings.h>
#include <momentum/asset_packs.h>
#include <storage/storage.h>

static const char* const menu_style_names[MenuStyleCount] = {
    "List",
    "Wii",
    "DSi",
    "PS4",
    "Vertical",
    "C64",
    "Compact",
    "MNTM",
    "CoverFlow",
};

static void momentum_scene_interface_graphics_asset_pack_callback(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->scene_manager->scene[app->scene_manager->scene_id_stack.data[0]].state = index;
    // This would trigger a pack change
    view_dispatcher_send_custom_event(app->view_dispatcher, 0);
}

static void momentum_scene_interface_graphics_menu_style_callback(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    momentum_settings.menu_style = index;
    variable_item_set_current_value_text(item, menu_style_names[index]);
}

static void build_graphics_menu(MomentumApp* app) {
    VariableItemList* var_item_list = app->var_item_list;
    VariableItem* item;

    variable_item_list_reset(var_item_list);

    // Find available asset packs
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = furi_string_alloc_set(ASSET_PACKS_PATH);
    File* dir = storage_file_alloc(storage);

    int pack_count = 1; // Default "None"
    if(storage_dir_open(dir, furi_string_get_cstr(path))) {
        FileInfo info;
        char name[64];
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            if(file_info_is_dir(&info)) pack_count++;
        }
        storage_dir_close(dir);
    }

    item = variable_item_list_add(
        var_item_list, "Asset Pack", pack_count, momentum_scene_interface_graphics_asset_pack_callback, app);
    variable_item_set_current_value_index(item, 0);
    variable_item_set_current_value_text(item, momentum_settings.asset_pack[0] ? momentum_settings.asset_pack : "None");

    item = variable_item_list_add(
        var_item_list, "Menu Style", MenuStyleCount, momentum_scene_interface_graphics_menu_style_callback, app);
    variable_item_set_current_value_index(item, momentum_settings.menu_style);
    variable_item_set_current_value_text(item, menu_style_names[momentum_settings.menu_style]);

    storage_file_free(dir);
    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);
}

void momentum_scene_interface_graphics_on_enter(void* context) {
    MomentumApp* app = context;
    VariableItemList* var_item_list = app->var_item_list;

    variable_item_list_set_enter_callback(var_item_list, NULL, NULL);
    build_graphics_menu(app);

    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumAppViewVarItemList);
}

bool momentum_scene_interface_graphics_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void momentum_scene_interface_graphics_on_exit(void* context) {
    MomentumApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
