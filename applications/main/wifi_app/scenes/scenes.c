#include "scenes.h"

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_enter,
void (*const wifi_app_on_enter_handlers[])(void*) = {
#include "scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_event,
bool (*const wifi_app_on_event_handlers[])(void* context, SceneManagerEvent event) = {
#include "scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_exit,
void (*const wifi_app_on_exit_handlers[])(void* context) = {
#include "scene_config.h"
};
#undef ADD_SCENE

const SceneManagerHandlers wifi_app_scene_handlers = {
    .on_enter_handlers = wifi_app_on_enter_handlers,
    .on_event_handlers = wifi_app_on_event_handlers,
    .on_exit_handlers = wifi_app_on_exit_handlers,
    .scene_num = WifiAppSceneNum,
};