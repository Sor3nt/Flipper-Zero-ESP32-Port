#include "../momentum_app.h"
#include "momentum_app_scene.h"
#include <momentum/momentum_settings.h>

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

static void momentum_scene_interface_mainmenu_style_callback(void* context, uint32_t index) {
    MomentumApp* app = context;
    if(index < MenuStyleCount) {
        momentum_settings.menu_style = index;
    }
    view_dispatcher_send_custom_event(app->view_dispatcher, 0);
}

void momentum_scene_interface_mainmenu_style_on_enter(void* context) {
    MomentumApp* app = context;
    Submenu* submenu = app->submenu;

    submenu_reset(submenu);
    for(int i = 0; i < MenuStyleCount; i++) {
        submenu_add_item(submenu, menu_style_names[i], i, momentum_scene_interface_mainmenu_style_callback, app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumAppViewSubmenu);
}

bool momentum_scene_interface_mainmenu_style_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void momentum_scene_interface_mainmenu_style_on_exit(void* context) {
    UNUSED(context);
}
