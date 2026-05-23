#include "../momentum_app.h"
#include "momentum_app_scene.h"
#include <momentum/momentum_settings.h>

void momentum_scene_interface_mainmenu_on_enter(void* context) {
    MomentumApp* app = context;
    Submenu* submenu = app->submenu;

    submenu_reset(submenu);
    submenu_add_item(submenu, "Menu Style", 0, NULL, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumAppViewSubmenu);
}

bool momentum_scene_interface_mainmenu_on_event(void* context, SceneManagerEvent event) {
    MomentumApp* app = context;
    if(event.type == SceneManagerEventTypeCustom && event.event == 0) {
        scene_manager_next_scene(app->scene_manager, MomentumSceneInterfaceMainmenuStyle);
        return true;
    }
    return false;
}

void momentum_scene_interface_mainmenu_on_exit(void* context) {
    UNUSED(context);
}
