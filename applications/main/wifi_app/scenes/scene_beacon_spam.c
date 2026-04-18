#include "../wifi_app.h"
#include "../wifi_hal.h"
#include "../views/beacon_view.h"

void wifi_app_scene_beacon_spam_on_enter(void* context) {
    WifiApp* app = context;
    
    if(!wifi_hal_is_started()) {
        wifi_hal_start();
    }
    
    wifi_hal_beacon_spam_start((WifiHalBeaconMode)app->beacon_mode, app->single_ssid);
    beacon_view_set_status(app->beacon_view_obj, "Spamming...");
    beacon_view_set_frame_count(app->beacon_view_obj, 0);
    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewBeacon);
}

bool wifi_app_scene_beacon_spam_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;
    
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == InputKeyOk) {
            wifi_hal_beacon_spam_stop();
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(wifi_hal_beacon_spam_is_running()) {
            beacon_view_set_frame_count(app->beacon_view_obj, wifi_hal_beacon_spam_get_frame_count());
        } else {
            beacon_view_set_status(app->beacon_view_obj, "Stopped");
        }
    }
    return consumed;
}

void wifi_app_scene_beacon_spam_on_exit(void* context) {
    UNUSED(context);
    wifi_hal_beacon_spam_stop();
}