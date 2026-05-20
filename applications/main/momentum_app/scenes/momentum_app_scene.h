#pragma once

#include <gui/scene_manager.h>

#define ADD_SCENE(prefix, name, id) MomentumScene##id,
typedef enum {
#include "momentum_app_scene_config.h"
    MomentumSceneNum,
} MomentumScene;
#undef ADD_SCENE

extern const SceneManagerHandlers momentum_scene_handlers;

#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_enter(void*);
#include "momentum_app_scene_config.h"
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) \
    bool prefix##_scene_##name##_on_event(void* context, SceneManagerEvent event);
#include "momentum_app_scene_config.h"
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_exit(void* context);
#include "momentum_app_scene_config.h"
#undef ADD_SCENE
