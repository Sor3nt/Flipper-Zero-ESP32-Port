#include "../bad_ble_app_i.h"

void bad_ble_scene_list_on_enter(void* context) {
    BadBleApp* app = context;
    UNUSED(app);
}

bool bad_ble_scene_list_on_event(void* context, SceneManagerEvent event) {
    BadBleApp* app = context;
    UNUSED(app);
    UNUSED(event);
    return false;
}

void bad_ble_scene_list_on_exit(void* context) {
    BadBleApp* app = context;
    UNUSED(app);
}
