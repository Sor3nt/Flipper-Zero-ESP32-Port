/* Web-Filesystem info scene (Widget): starts the file server (AP or STA per
 * scene state: 1 = AP, 0 = STA) and shows SSID + URL + client count. Back stops
 * the server and returns to the entry menu. */

#include "../wlan_app.h"

static int s_last_clients = -1;

static void webfs_info_button_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventWebFsStop);
    }
}

static void webfs_info_render(WlanApp* app, bool ok) {
    Widget* w = app->widget;
    widget_reset(w);
    widget_add_string_element(w, 64, 2, AlignCenter, AlignTop, FontPrimary, "Web-Filesystem");

    if(!ok) {
        widget_add_string_element(
            w, 64, 30, AlignCenter, AlignCenter, FontSecondary, "Start failed (RAM/WiFi?)");
        widget_add_button_element(w, GuiButtonTypeCenter, "Exit", webfs_info_button_cb, app);
        return;
    }

    bool ap = wlan_webfs_is_ap();
    char line[64];
    char ip[16] = {0};
    wlan_webfs_get_ip(ip, sizeof(ip));

    const char* ssid = ap ? app->webfs_ssid : app->connected_ap.ssid;
    snprintf(line, sizeof(line), "SSID: %s", ssid);
    widget_add_string_element(w, 64, 18, AlignCenter, AlignTop, FontSecondary, line);

    snprintf(line, sizeof(line), "http://%s", ip);
    widget_add_string_element(w, 64, 30, AlignCenter, AlignTop, FontPrimary, line);

    if(ap) {
        snprintf(line, sizeof(line), "Clients: %u", (unsigned)wlan_webfs_get_client_count());
        widget_add_string_element(w, 64, 44, AlignCenter, AlignTop, FontSecondary, line);
    }

    widget_add_button_element(w, GuiButtonTypeCenter, "Stop", webfs_info_button_cb, app);
}

/* Stop happens in on_exit; return to the entry scene. */
static void webfs_info_leave(WlanApp* app) {
    if(scene_manager_search_and_switch_to_previous_scene(
           app->scene_manager, WlanAppSceneWebFsMenu))
        return;
    if(scene_manager_search_and_switch_to_previous_scene(
           app->scene_manager, WlanAppSceneMain))
        return;

    scene_manager_stop(app->scene_manager);
    view_dispatcher_stop(app->view_dispatcher);
}

void wlan_app_scene_webfs_info_on_enter(void* context) {
    WlanApp* app = context;
    uint32_t mode = scene_manager_get_scene_state(app->scene_manager, WlanAppSceneWebFsInfo);

    bool ok = (mode == 1) ? wlan_webfs_start_ap(app->webfs_ssid, app->webfs_pw) :
                            wlan_webfs_start_sta();

    s_last_clients = -1;
    webfs_info_render(app, ok);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

bool wlan_app_scene_webfs_info_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventWebFsStop) {
            webfs_info_leave(app);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        // Refresh the client count in AP mode when it changes.
        if(wlan_webfs_is_running() && wlan_webfs_is_ap()) {
            int now = wlan_webfs_get_client_count();
            if(now != s_last_clients) {
                s_last_clients = now;
                webfs_info_render(app, true);
            }
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeBack) {
        webfs_info_leave(app);
        consumed = true;
    }
    return consumed;
}

void wlan_app_scene_webfs_info_on_exit(void* context) {
    UNUSED(context);
    wlan_webfs_stop();
}
