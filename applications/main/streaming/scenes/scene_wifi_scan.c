#include "../streaming.h"

#include <wlan_hal.h>
#include <wlan_passwords.h>
#include <string.h>

static void wifi_scan_submenu_cb(void* ctx, uint32_t index) {
    StreamingApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void run_scan(StreamingApp* app) {
    app->ap_count = 0;
    if(!wlan_hal_is_started()) {
        if(!wlan_hal_start()) return;
    }

    wifi_ap_record_t* raw = NULL;
    uint16_t found = 0;
    wlan_hal_scan(&raw, &found, STREAMING_MAX_APS);

    for(uint16_t i = 0; i < found && app->ap_count < STREAMING_MAX_APS; ++i) {
        if(raw[i].ssid[0] == '\0') continue; /* skip hidden APs */
        StreamingApRecord* r = &app->ap_records[app->ap_count++];
        memset(r, 0, sizeof(*r));
        strncpy(r->ssid, (const char*)raw[i].ssid, sizeof(r->ssid) - 1);
        memcpy(r->bssid, raw[i].bssid, 6);
        r->rssi = raw[i].rssi;
        r->channel = raw[i].primary;
        r->authmode = raw[i].authmode;
        r->is_open = (raw[i].authmode == WIFI_AUTH_OPEN);
        r->has_password = !r->is_open && wlan_password_exists(r->ssid);
    }
    if(raw) free(raw);
}

void streaming_scene_wifi_scan_on_enter(void* context) {
    StreamingApp* app = context;

    view_dispatcher_switch_to_view(app->view_dispatcher, StreamingViewLoading);
    run_scan(app); /* blocking; loading view shows once we yield */

    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Select WiFi");
    for(uint16_t i = 0; i < app->ap_count; ++i) {
        submenu_add_item(app->submenu, app->ap_records[i].ssid, i, wifi_scan_submenu_cb, app);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, StreamingViewSubmenu);
}

bool streaming_scene_wifi_scan_on_event(void* context, SceneManagerEvent event) {
    StreamingApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event < app->ap_count) {
        app->ap_selected_index = event.event;
        app->target_ap = app->ap_records[event.event];
        scene_manager_next_scene(app->scene_manager, StreamingSceneWifiConnect);
        return true;
    }
    return false;
}

void streaming_scene_wifi_scan_on_exit(void* context) {
    StreamingApp* app = context;
    submenu_reset(app->submenu);
}
