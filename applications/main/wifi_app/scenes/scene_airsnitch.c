#include "../wifi_app.h"
#include "../wifi_hal.h"
#include "../wifi_passwords.h"

#include <esp_log.h>

#define TAG WIFI_APP_LOG_TAG

void wifi_app_scene_airsnitch_on_enter(void* context) {
    WifiApp* app = context;

    // Show loading while connecting
    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewLoading);

    if(!wifi_hal_is_started()) {
        wifi_hal_start();
    }

    // Get selected AP
    WifiApRecord* target = NULL;
    if(app->selected_index < app->ap_count) {
        target = &app->ap_records[app->selected_index];
    }

    if(!target) {
        ESP_LOGE(TAG, "AirSnitch: no AP selected");
        scene_manager_previous_scene(app->scene_manager);
        return;
    }

    // Get password from SD card
    const char* password = NULL;
    char pw_buf[65] = {0};
    if(!target->is_open) {
        if(wifi_password_read(target->ssid, pw_buf, sizeof(pw_buf))) {
            password = pw_buf;
        } else {
            ESP_LOGW(TAG, "AirSnitch: no password for '%s', trying open", target->ssid);
        }
    }

    // Connect
    ESP_LOGI(TAG, "Connecting to '%s' (open=%d, has_pw=%d, authmode=%d, rssi=%d, ch=%d, bssid=%02X:%02X:%02X:%02X:%02X:%02X)",
             target->ssid, target->is_open, target->has_password, target->authmode,
             target->rssi, target->channel,
             target->bssid[0], target->bssid[1], target->bssid[2],
             target->bssid[3], target->bssid[4], target->bssid[5]);
    wifi_hal_connect(target->ssid, password, target->bssid, target->channel);

    // Wait for connection (poll with timeout)
    bool connected = false;
    for(int i = 0; i < 50; i++) { // 50 * 100ms = 5s timeout
        furi_delay_ms(100);
        if(wifi_hal_is_connected()) {
            connected = true;
            break;
        }
    }

    if(connected) {
        ESP_LOGI(TAG, "AirSnitch: connected, starting scan");
        scene_manager_next_scene(app->scene_manager, WifiAppSceneAirsnScan);
    } else {
        ESP_LOGE(TAG, "AirSnitch: connection failed");
        // Show error
        widget_reset(app->widget);
        widget_add_string_element(
            app->widget, 64, 20, AlignCenter, AlignCenter, FontPrimary, "Connection Failed");
        widget_add_string_element(
            app->widget, 64, 40, AlignCenter, AlignCenter, FontSecondary, target->ssid);
        view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewWidget);
    }
}

bool wifi_app_scene_airsnitch_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void wifi_app_scene_airsnitch_on_exit(void* context) {
    WifiApp* app = context;
    widget_reset(app->widget);
}
