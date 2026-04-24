#include "../bad_ble_app_i.h"

void bad_ble_scene_run_on_enter(void* context) {
    BadBleApp* app = context;
    app->is_running = true;
}

bool bad_ble_scene_run_on_event(void* context, SceneManagerEvent event) {
    BadBleApp* app = context;
    UNUSED(app);
    UNUSED(event);
    return false;
}

void bad_ble_scene_run_on_exit(void* context) {
    BadBleApp* app = context;
    app->is_running = false;
}
