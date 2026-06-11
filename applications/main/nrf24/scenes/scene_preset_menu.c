#include "../nrf24_app.h"
#include "../helpers/nrf24_jam_presets.h"

static void preset_menu_callback(void* context, uint32_t index) {
    Nrf24App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void nrf24_app_scene_preset_menu_on_enter(void* context) {
    Nrf24App* app = context;

    submenu_set_header(app->submenu, "Presets");
    for(uint32_t i = 0; i < Nrf24JamPresetCount; i++) {
        submenu_add_item(
            app->submenu, nrf24_jam_preset_name(i), i, preset_menu_callback, app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewSubmenu);
}

bool nrf24_app_scene_preset_menu_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event < Nrf24JamPresetCount) {
        app->selected_jam_preset = (uint8_t)event.event;
        scene_manager_next_scene(app->scene_manager, Nrf24AppScenePresetJam);
        return true;
    }
    return false;
}

void nrf24_app_scene_preset_menu_on_exit(void* context) {
    Nrf24App* app = context;
    submenu_reset(app->submenu);
}
