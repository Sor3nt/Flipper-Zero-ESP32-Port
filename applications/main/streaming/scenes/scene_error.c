#include "../streaming.h"

/* Generic error scene: shows app->error_msg in a popup. Back returns to the
 * previous scene (e.g. the device list). Used e.g. when an AirPlay receiver
 * requires encryption (et>0), which the PCM-only RAOP sender cannot do. */

void streaming_scene_error_on_enter(void* context) {
    StreamingApp* app = context;
    popup_reset(app->popup);
    popup_set_header(app->popup, "Not supported", 64, 12, AlignCenter, AlignTop);
    popup_set_text(app->popup, app->error_msg, 64, 36, AlignCenter, AlignCenter);
    view_dispatcher_switch_to_view(app->view_dispatcher, StreamingViewPopup);
}

bool streaming_scene_error_on_event(void* context, SceneManagerEvent event) {
    StreamingApp* app = context;
    if(event.type == SceneManagerEventTypeBack) {
        /* Return to the device list, SKIPPING the player scene: its on_enter
         * would immediately re-run the failed AirPlay handshake and bounce
         * straight back here (error loop). Falls back to normal Back if the
         * device scan isn't in the stack (e.g. et>0 error came from it directly,
         * where it's already the previous scene anyway). */
        if(scene_manager_search_and_switch_to_previous_scene(
               app->scene_manager, StreamingSceneDeviceScan)) {
            return true;
        }
        return false;
    }
    return false;
}

void streaming_scene_error_on_exit(void* context) {
    StreamingApp* app = context;
    popup_reset(app->popup);
}
