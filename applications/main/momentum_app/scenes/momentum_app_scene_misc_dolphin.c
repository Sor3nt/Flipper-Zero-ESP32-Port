#include "../momentum_app.h"
#include "momentum_app_scene.h"

void momentum_scene_misc_dolphin_on_enter(void* context) {
    MomentumApp* app = context;
    UNUSED(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumAppViewVarItemList);
}

bool momentum_scene_misc_dolphin_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void momentum_scene_misc_dolphin_on_exit(void* context) {
    UNUSED(context);
}
