#include "bw16_scene.h"

// Generate scene on_enter handlers array
#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_enter,
void (*const bw16_scene_on_enter_handlers[])(void*) = {
#include "bw16_scene_config.h"
};
#undef ADD_SCENE

// Generate scene on_event handlers array
#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_event,
bool (*const bw16_scene_on_event_handlers[])(void* context, SceneManagerEvent event) = {
#include "bw16_scene_config.h"
};
#undef ADD_SCENE

// Generate scene on_exit handlers array
#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_exit,
void (*const bw16_scene_on_exit_handlers[])(void* context) = {
#include "bw16_scene_config.h"
};
#undef ADD_SCENE

const SceneManagerHandlers bw16_scene_handlers = {
    .on_enter_handlers = bw16_scene_on_enter_handlers,
    .on_event_handlers = bw16_scene_on_event_handlers,
    .on_exit_handlers = bw16_scene_on_exit_handlers,
    .scene_num = Bw16SceneNum,
};
