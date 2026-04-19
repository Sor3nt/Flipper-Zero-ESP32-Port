#include "../esp_now_app.h"

enum SubmenuIndex {
    SubmenuIndexSniff,
};

static void esp_now_scene_menu_submenu_callback(void* context, uint32_t index) {
    EspNowApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void esp_now_app_scene_menu_on_enter(void* context) {
    EspNowApp* app = context;

    submenu_add_item(
        app->submenu, "Sniff", SubmenuIndexSniff, esp_now_scene_menu_submenu_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, EspNowViewSubmenu);
}

bool esp_now_app_scene_menu_on_event(void* context, SceneManagerEvent event) {
    EspNowApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubmenuIndexSniff) {
            scene_manager_next_scene(app->scene_manager, EspNowAppSceneSniff);
            consumed = true;
        }
    }

    return consumed;
}

void esp_now_app_scene_menu_on_exit(void* context) {
    EspNowApp* app = context;
    submenu_reset(app->submenu);
}
