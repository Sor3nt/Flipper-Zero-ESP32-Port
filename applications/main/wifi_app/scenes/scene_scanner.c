#include "../wifi_app.h"
#include "../wifi_hal.h"
#include "../wifi_passwords.h"
#include "../views/ap_list.h"

#include <esp_wifi.h>
#include <esp_log.h>
#include <string.h>

#define TAG WIFI_APP_LOG_TAG

void wifi_app_scene_scanner_on_enter(void* context) {
    WifiApp* app = context;
    app->ap_count = 0;

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewLoading);

    if(!wifi_hal_is_started()) {
        if(!wifi_hal_start()) {
            ESP_LOGE(TAG, "WiFi start failed");
            ApListModel* model = view_get_model(app->view_ap_list);
            model->records = NULL;
            model->count = 0;
            view_commit_model(app->view_ap_list, true);
            view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewApList);
            return;
        }
    }

    // Scan via worker task
    ESP_LOGI(TAG, "Scanning...");
    wifi_ap_record_t* raw = NULL;
    uint16_t ap_count = 0;
    wifi_hal_scan(&raw, &ap_count, WIFI_APP_MAX_APS);

    ESP_LOGI(TAG, "Found %d APs", ap_count);
    app->ap_count = ap_count;
    for(uint16_t i = 0; i < ap_count; i++) {
        strncpy(app->ap_records[i].ssid, (const char*)raw[i].ssid, 32);
        app->ap_records[i].ssid[32] = '\0';
        memcpy(app->ap_records[i].bssid, raw[i].bssid, 6);
        app->ap_records[i].rssi = raw[i].rssi;
        app->ap_records[i].channel = raw[i].primary;
        app->ap_records[i].authmode = raw[i].authmode;
        app->ap_records[i].is_open = (raw[i].authmode == WIFI_AUTH_OPEN);
        app->ap_records[i].has_password = wifi_password_exists(app->ap_records[i].ssid);
    }
    if(raw) free(raw);

    // Populate AP list view
    ApListModel* model = view_get_model(app->view_ap_list);
    model->records = (ApListRecord*)app->ap_records;
    model->count = app->ap_count;
    model->selected = 0;
    model->window_offset = 0;
    view_commit_model(app->view_ap_list, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewApList);
}

bool wifi_app_scene_scanner_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == 100) { // AP_LIST_EVENT_SELECTED
            ApListModel* model = view_get_model(app->view_ap_list);
            app->selected_index = model->selected;
            view_commit_model(app->view_ap_list, false);

            uint8_t next = app->scanner_next_scene;
            app->scanner_next_scene = WifiAppSceneApDetail; // reset for next time
            scene_manager_next_scene(app->scene_manager, next);
            consumed = true;
        }
    }

    return consumed;
}

void wifi_app_scene_scanner_on_exit(void* context) {
    UNUSED(context);
}
