#include <gui/scene_manager.h>
#include <loader/loader.h>
#include <esp_netif.h>
#include <esp_wifi.h>

#include "../desktop_i.h"
#include "../views/desktop_view_lock_menu.h"
#include "desktop_scene.h"

void desktop_scene_lock_menu_callback(DesktopEvent event, void* context) {
    Desktop* desktop = (Desktop*)context;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, event);
}

void desktop_scene_lock_menu_on_enter(void* context) {
    Desktop* desktop = (Desktop*)context;

    // Check WiFi connection state
    bool connected = false;
    char ip_str[16] = {0};

    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(netif) {
        esp_netif_ip_info_t ip_info = {0};
        if(esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            connected = true;
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        }
    }

    desktop_lock_menu_set_callback(desktop->lock_menu, desktop_scene_lock_menu_callback, desktop);
    desktop_lock_menu_set_wifi_state(desktop->lock_menu, connected, ip_str);

    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdLockMenu);
}

bool desktop_scene_lock_menu_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = (Desktop*)context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case DesktopLockMenuEventSubGhz:
            loader_start_detached_with_gui_error(desktop->loader, "subghz", NULL);
            consumed = true;
            break;
        case DesktopLockMenuEventConnectWifi:
            loader_start_detached_with_gui_error(desktop->loader, "wifi", NULL);
            consumed = true;
            break;
        case DesktopLockMenuEventDisconnectWifi:
            esp_wifi_disconnect();
            esp_wifi_stop();
            // Return to desktop
            scene_manager_search_and_switch_to_previous_scene(
                desktop->scene_manager, DesktopSceneMain);
            consumed = true;
            break;
        case DesktopLockMenuEventHandshake:
            loader_start_detached_with_gui_error(desktop->loader, "wifi", "handshake_channel");
            consumed = true;
            break;
        case DesktopLockMenuEventDeauth:
            loader_start_detached_with_gui_error(desktop->loader, "wifi", "channel_deauth");
            consumed = true;
            break;
        default:
            break;
        }
    }

    return consumed;
}

void desktop_scene_lock_menu_on_exit(void* context) {
    UNUSED(context);
}
