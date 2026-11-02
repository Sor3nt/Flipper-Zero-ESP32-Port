#pragma once

#include <gui/scene_manager.h>

// Generate scene id enum
#define ADD_SCENE(prefix, name, id) Bw16Scene##id,
typedef enum {
#include "bw16_scene_config.h"
    Bw16SceneNum,
} Bw16Scene;
#undef ADD_SCENE

extern const SceneManagerHandlers bw16_scene_handlers;

// Generate scene on_enter handlers declaration
#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_enter(void*);
#include "bw16_scene_config.h"
#undef ADD_SCENE

// Generate scene on_event handlers declaration
#define ADD_SCENE(prefix, name, id) \
    bool prefix##_scene_##name##_on_event(void* context, SceneManagerEvent event);
#include "bw16_scene_config.h"
#undef ADD_SCENE

// Generate scene on_exit handlers declaration
#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_exit(void* context);
#include "bw16_scene_config.h"
#undef ADD_SCENE
