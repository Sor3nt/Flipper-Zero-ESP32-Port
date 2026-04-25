#include "../wifi_app.h"
#include "../wifi_hal.h"
#include "../wifi_passwords.h"

#include <esp_netif.h>
#include <esp_log.h>

#define TAG WIFI_APP_LOG_TAG

static uint32_t s_tick_count;
static uint32_t s_done_tick;  // tick when connect finished (success or fail)
static bool s_connect_success;
static bool s_connect_done;
static char s_used_password[65];  // password used for this connection attempt

void wifi_app_scene_connect_on_enter(void* context) {
    WifiApp* app = context;

    s_tick_count = 0;
    s_done_tick = 0;
    s_connect_success = false;
    s_connect_done = false;
    s_used_password[0] = '\0';

    widget_reset(app->widget);
    widget_add_string_multiline_element(
        app->widget, 64, 28, AlignCenter, AlignCenter, FontPrimary, "Connecting...");
    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewWidget);

    if(!app->ap_selected) {
        ESP_LOGE(TAG, "No AP selected");
        s_connect_done = true;
        return;
    }

    WifiApRecord* ap = &app->connected_ap;

    char password[65] = {0};
    if(app->password_input[0]) {
        strncpy(password, app->password_input, sizeof(password) - 1);
        app->password_input[0] = '\0';
    } else if(ap->has_password) {
        wifi_password_read(ap->ssid, password, sizeof(password));
    }

    strncpy(s_used_password, password, sizeof(s_used_password) - 1);

    ESP_LOGI(TAG, "Connecting to '%s' (open=%d, has_pw=%d)", ap->ssid, ap->is_open, ap->has_password);

    if(!wifi_hal_connect(ap->ssid, ap->is_open ? "" : password, ap->bssid, ap->channel)) {
        ESP_LOGE(TAG, "Connect command failed");
        s_connect_done = true;
    }
}

bool wifi_app_scene_connect_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        s_tick_count++;

        if(!s_connect_done && wifi_hal_is_connected()) {
            s_connect_success = true;
            s_connect_done = true;

            WifiApRecord* ap = &app->connected_ap;

            if(s_used_password[0] && !ap->is_open) {
                wifi_password_save(ap->ssid, s_used_password);
                ap->has_password = true;
            }

            ESP_LOGI(TAG, "WiFi connected to '%s'", ap->ssid);

            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, WifiAppSceneMenu);
            scene_manager_next_scene(app->scene_manager, WifiAppSceneSsidAttackMenu);
            consumed = true;
            return consumed;
        }

        if(!s_connect_done && s_tick_count > 40) {
            // 10 second timeout
            s_connect_done = true;
            s_done_tick = s_tick_count;
            wifi_hal_disconnect();

            widget_reset(app->widget);
            widget_add_string_multiline_element(
                app->widget, 64, 28, AlignCenter, AlignCenter, FontPrimary, "Failed!");
            ESP_LOGW(TAG, "Connect timeout");
        }

        // Failure: show "Failed!" briefly, then go back
        if(s_connect_done && !s_connect_success && s_done_tick > 0 &&
           (s_tick_count - s_done_tick) >= 6) {
            scene_manager_previous_scene(app->scene_manager);
        }

        consumed = true;
    }

    return consumed;
}

void wifi_app_scene_connect_on_exit(void* context) {
    WifiApp* app = context;
    widget_reset(app->widget);
}
