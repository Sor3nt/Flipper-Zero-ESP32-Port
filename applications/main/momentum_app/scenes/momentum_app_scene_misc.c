#include "../momentum_app.h"
#include "momentum_app_scene.h"

static void momentum_scene_misc_submenu_callback(void* context, uint32_t index) {
    MomentumApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void momentum_scene_misc_on_enter(void* context) {
    MomentumApp* app = context;
    Submenu* submenu = app->submenu;

    submenu_reset(submenu);
    submenu_add_item(submenu, "Screen", 0, momentum_scene_misc_submenu_callback, app);
    submenu_add_item(submenu, "Spoofing", 1, momentum_scene_misc_submenu_callback, app);
    submenu_add_item(submenu, "Dolphin", 2, momentum_scene_misc_submenu_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumAppViewSubmenu);
}

bool momentum_scene_misc_on_event(void* context, SceneManagerEvent event) {
    MomentumApp* app = context;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case 0:
            scene_manager_next_scene(app->scene_manager, MomentumSceneMiscScreen);
            return true;
        case 1:
            scene_manager_next_scene(app->scene_manager, MomentumSceneMiscSpoof);
            return true;
        case 2:
            scene_manager_next_scene(app->scene_manager, MomentumSceneMiscDolphin);
            return true;
        }
    }

    return false;
}

void momentum_scene_misc_on_exit(void* context) {
    UNUSED(context);
}
