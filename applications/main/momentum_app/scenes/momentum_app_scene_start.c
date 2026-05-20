#include "../momentum_app.h"
#include "momentum_app_scene.h"
#include <momentum/momentum_settings.h>

static void momentum_scene_start_submenu_callback(void* context, uint32_t index) {
    MomentumApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void momentum_scene_start_on_enter(void* context) {
    MomentumApp* app = context;
    Submenu* submenu = app->submenu;

    submenu_add_item(submenu, "Interface", 0, momentum_scene_start_submenu_callback, app);
    submenu_add_item(submenu, "Protocols", 1, momentum_scene_start_submenu_callback, app);
    submenu_add_item(submenu, "Misc", 2, momentum_scene_start_submenu_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumAppViewSubmenu);
}

bool momentum_scene_start_on_event(void* context, SceneManagerEvent event) {
    MomentumApp* app = context;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == 0) {
            scene_manager_next_scene(app->scene_manager, MomentumSceneInterface);
            return true;
        } else if(event.event == 1) {
            scene_manager_next_scene(app->scene_manager, MomentumSceneProtocols);
            return true;
        } else if(event.event == 2) {
            scene_manager_next_scene(app->scene_manager, MomentumSceneMisc);
            return true;
        }
    }

    return false;
}

void momentum_scene_start_on_exit(void* context) {
    MomentumApp* app = context;
    submenu_reset(app->submenu);
}
