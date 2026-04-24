#include "bad_ble_app_i.h"

// Scene handler arrays
#define ADD_SCENE(name, id) bad_ble_scene_##name##_on_enter,
void (*const bad_ble_scene_on_enter_handlers[])(void*) = {
    ADD_SCENE(start, BadBleSceneStart)
    ADD_SCENE(settings, BadBleSceneSettings)
    ADD_SCENE(run, BadBleSceneRun)
    ADD_SCENE(list, BadBleSceneList)
};
#undef ADD_SCENE

#define ADD_SCENE(name, id) bad_ble_scene_##name##_on_event,
bool (*const bad_ble_scene_on_event_handlers[])(void* context, SceneManagerEvent event) = {
    ADD_SCENE(start, BadBleSceneStart)
    ADD_SCENE(settings, BadBleSceneSettings)
    ADD_SCENE(run, BadBleSceneRun)
    ADD_SCENE(list, BadBleSceneList)
};
#undef ADD_SCENE

#define ADD_SCENE(name, id) bad_ble_scene_##name##_on_exit,
void (*const bad_ble_scene_on_exit_handlers[])(void* context) = {
    ADD_SCENE(start, BadBleSceneStart)
    ADD_SCENE(settings, BadBleSceneSettings)
    ADD_SCENE(run, BadBleSceneRun)
    ADD_SCENE(list, BadBleSceneList)
};
#undef ADD_SCENE

const SceneManagerHandlers bad_ble_scene_handlers = {
    .on_enter_handlers = bad_ble_scene_on_enter_handlers,
    .on_event_handlers = bad_ble_scene_on_event_handlers,
    .on_exit_handlers = bad_ble_scene_on_exit_handlers,
    .scene_num = BadBleSceneCount,
};
