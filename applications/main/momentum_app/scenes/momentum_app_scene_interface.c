#include "../momentum_app.h"
#include "momentum_app_scene.h"
#include <momentum/momentum_settings.h>

static void momentum_scene_interface_submenu_callback(void* context, uint32_t index) {
    MomentumApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void momentum_scene_interface_on_enter(void* context) {
    MomentumApp* app = context;
    Submenu* submenu = app->submenu;

    submenu_add_item(submenu, "Graphics", 0, momentum_scene_interface_submenu_callback, app);
    submenu_add_item(submenu, "Main Menu", 1, momentum_scene_interface_submenu_callback, app);
    submenu_add_item(submenu, "Lock Screen", 2, momentum_scene_interface_submenu_callback, app);
    submenu_add_item(submenu, "Status Bar", 3, momentum_scene_interface_submenu_callback, app);
    submenu_add_item(submenu, "File Browser", 4, momentum_scene_interface_submenu_callback, app);
    submenu_add_item(submenu, "General", 5, momentum_scene_interface_submenu_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumAppViewSubmenu);
}

bool momentum_scene_interface_on_event(void* context, SceneManagerEvent event) {
    MomentumApp* app = context;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case 0:
            scene_manager_next_scene(app->scene_manager, MomentumSceneInterfaceGraphics);
            return true;
        case 1:
            scene_manager_next_scene(app->scene_manager, MomentumSceneInterfaceMainmenu);
            return true;
        case 2:
            scene_manager_next_scene(app->scene_manager, MomentumSceneInterfaceLockscreen);
            return true;
        case 3:
            scene_manager_next_scene(app->scene_manager, MomentumSceneInterfaceStatusbar);
            return true;
        case 4:
            scene_manager_next_scene(app->scene_manager, MomentumSceneInterfaceFilebrowser);
            return true;
        case 5:
            scene_manager_next_scene(app->scene_manager, MomentumSceneInterfaceGeneral);
            return true;
        }
    }

    return false;
}

void momentum_scene_interface_on_exit(void* context) {
    MomentumApp* app = context;
    submenu_reset(app->submenu);
}
