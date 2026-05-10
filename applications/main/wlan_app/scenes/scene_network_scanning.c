#include "../wlan_app.h"
#include "../wlan_netscan.h"

#include <lwip/ip4_addr.h>

typedef enum {
    NetScanStateArp = 0,
    NetScanStateHostnames,
    NetScanStateRetryDialog,
    NetScanStateDone,
} NetScanState;

#define NET_SCAN_ARP_DURATION_TICKS 80   // 80 * 250 ms ≈ 20 s (254 IPs / 4 per tick = 64 ticks + Reserve)
#define NET_SCAN_HOSTNAME_TIMEOUT_TICKS 60 // ~15 s

static uint16_t s_tick_counter;
static bool s_popup_shown;
static bool s_dialog_shown;

static void net_scan_set_state(WlanApp* app, NetScanState state);

static void net_scan_close_overlays(WlanApp* app) {
    if(s_popup_shown) {
        popup_reset(app->popup);
        s_popup_shown = false;
    }
    if(s_dialog_shown) {
        widget_reset(app->widget);
        s_dialog_shown = false;
    }
}

static void net_scan_show_popup(WlanApp* app, const char* text) {
    net_scan_close_overlays(app);
    popup_reset(app->popup);
    popup_set_header(app->popup, "Network Scan", 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, text, 64, 32, AlignCenter, AlignCenter);
    popup_set_context(app->popup, app);
    popup_disable_timeout(app->popup);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPopup);
    s_popup_shown = true;
}

static void net_scan_retry_yes_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, WlanAppCustomEventNetworkScanRetry);
    }
}

static void net_scan_retry_no_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeLeft) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, WlanAppCustomEventNetworkScanCancel);
    }
}

static void net_scan_show_retry_dialog(WlanApp* app) {
    net_scan_close_overlays(app);
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 22, AlignCenter, AlignBottom, FontPrimary, "No Devices found");
    widget_add_string_element(
        app->widget, 64, 38, AlignCenter, AlignBottom, FontSecondary, "Retry?");
    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "No", net_scan_retry_no_cb, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeRight, "Yes", net_scan_retry_yes_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
    s_dialog_shown = true;
}

static void net_scan_set_state(WlanApp* app, NetScanState state) {
    scene_manager_set_scene_state(app->scene_manager, WlanAppSceneNetworkScanning, state);
    s_tick_counter = 0;
    switch(state) {
    case NetScanStateArp:
        wlan_netscan_reset();
        net_scan_show_popup(app, "ARP Scan ...");
        break;
    case NetScanStateHostnames:
        net_scan_show_popup(app, "Resolve Hostnames ...");
        wlan_netscan_start_hostname_resolve();
        break;
    case NetScanStateRetryDialog:
        net_scan_show_retry_dialog(app);
        break;
    case NetScanStateDone:
        break;
    }
}

static void net_scan_publish_devices(WlanApp* app) {
    uint8_t count = wlan_netscan_get_host_count();
    if(count > WLAN_APP_MAX_DEVICES) count = WLAN_APP_MAX_DEVICES;
    app->device_count = count;
    for(uint8_t i = 0; i < count; ++i) {
        WlanNetscanHost h;
        if(!wlan_netscan_get_host(i, &h)) continue;
        WlanDeviceRecord* d = &app->devices[i];
        memset(d, 0, sizeof(*d));
        d->ip = h.ip;
        memcpy(d->mac, h.mac, 6);
        if(h.hostname[0]) {
            strncpy(d->hostname, h.hostname, sizeof(d->hostname) - 1);
        }
    }
}

void wlan_app_scene_network_scanning_on_enter(void* context) {
    WlanApp* app = context;
    s_tick_counter = 0;
    s_popup_shown = false;
    s_dialog_shown = false;
    app->device_count = 0;
    net_scan_set_state(app, NetScanStateArp);
}

bool wlan_app_scene_network_scanning_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventNetworkScanComplete) {
            app->lan_scan_complete = true;
            scene_manager_next_scene(app->scene_manager, WlanAppSceneLan);
            consumed = true;
        } else if(event.event == WlanAppCustomEventNetworkScanRetry) {
            net_scan_set_state(app, NetScanStateArp);
            consumed = true;
        } else if(event.event == WlanAppCustomEventNetworkScanCancel) {
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, WlanAppSceneMain);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        NetScanState st = (NetScanState)scene_manager_get_scene_state(
            app->scene_manager, WlanAppSceneNetworkScanning);

        if(st == NetScanStateArp) {
            bool arp_done = wlan_netscan_arp_step();
            s_tick_counter++;
            if(arp_done || s_tick_counter >= NET_SCAN_ARP_DURATION_TICKS) {
                net_scan_publish_devices(app);
                if(wlan_netscan_get_host_count() == 0) {
                    net_scan_set_state(app, NetScanStateRetryDialog);
                } else {
                    net_scan_set_state(app, NetScanStateHostnames);
                }
            }
        } else if(st == NetScanStateHostnames) {
            s_tick_counter++;
            if(wlan_netscan_hostname_done() ||
               s_tick_counter >= NET_SCAN_HOSTNAME_TIMEOUT_TICKS) {
                wlan_netscan_stop_hostname_resolve();
                net_scan_publish_devices(app);
                view_dispatcher_send_custom_event(
                    app->view_dispatcher, WlanAppCustomEventNetworkScanComplete);
            }
        }
    }

    return consumed;
}

void wlan_app_scene_network_scanning_on_exit(void* context) {
    WlanApp* app = context;
    wlan_netscan_stop_hostname_resolve();
    net_scan_close_overlays(app);
    s_tick_counter = 0;
    scene_manager_set_scene_state(app->scene_manager, WlanAppSceneNetworkScanning, NetScanStateArp);
}
