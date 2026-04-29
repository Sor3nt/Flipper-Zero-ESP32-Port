#include "../nrf24_app.h"

enum {
    MjMenuIndexManual = 1,
    MjMenuIndexAuto,
};

static void mj_menu_callback(void* context, uint32_t index) {
    Nrf24App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void nrf24_app_scene_mj_menu_on_enter(void* context) {
    Nrf24App* app = context;

    submenu_set_header(app->submenu, "MouseJacker");
    submenu_add_item(app->submenu, "by Target", MjMenuIndexManual, mj_menu_callback, app);
    submenu_add_item(app->submenu, "automatic", MjMenuIndexAuto, mj_menu_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewSubmenu);
}

bool nrf24_app_scene_mj_menu_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(event.event) {
    case MjMenuIndexManual:
        app->mj_auto_mode = false;
        app->mj_target_count = 0;
        app->mj_selected_target = -1;
        scene_manager_next_scene(app->scene_manager, Nrf24AppSceneMjScan);
        return true;
    case MjMenuIndexAuto:
        app->mj_auto_mode = true;
        app->mj_target_count = 0;
        app->mj_selected_target = -1;
        scene_manager_next_scene(app->scene_manager, Nrf24AppSceneMjScriptSelect);
        return true;
    default:
        return false;
    }
}

void nrf24_app_scene_mj_menu_on_exit(void* context) {
    Nrf24App* app = context;
    submenu_reset(app->submenu);
}
