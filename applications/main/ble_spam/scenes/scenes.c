#include "scenes.h"

// Generate scene on_enter handlers array
#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_enter,
static void (*const ble_spam_scene_on_enter_handlers[])(void*) = {
#include "scene_config.h"
};
#undef ADD_SCENE

// Generate scene on_event handlers array
#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_event,
static bool (*const ble_spam_scene_on_event_handlers[])(void*, SceneManagerEvent) = {
#include "scene_config.h"
};
#undef ADD_SCENE

// Generate scene on_exit handlers array
#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_exit,
static void (*const ble_spam_scene_on_exit_handlers[])(void*) = {
#include "scene_config.h"
};
#undef ADD_SCENE

const SceneManagerHandlers ble_spam_scene_handlers = {
    .on_enter_handlers = ble_spam_scene_on_enter_handlers,
    .on_event_handlers = ble_spam_scene_on_event_handlers,
    .on_exit_handlers = ble_spam_scene_on_exit_handlers,
    .scene_num = BleSpamSceneNum,
};
