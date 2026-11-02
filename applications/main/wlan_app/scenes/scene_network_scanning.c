#include "../wlan_app.h"
#include "../wlan_netscan.h"
#include "../wlan_lan_cache.h"

#include <lwip/ip4_addr.h>

typedef enum {
    NetScanStateArp = 0,
    NetScanStateHostnames,
    NetScanStateRetryDialog,
    NetScanStateDone,
} NetScanState;

#define NET_SCAN_ARP_DURATION_TICKS 80    // ~20 s @ 250ms tick
#define NET_SCAN_HOSTNAME_TIMEOUT_TICKS 60 // ~15 s @ 250ms tick

// ============================================================================
// STATE GLOBAL (file-scope, hanya diakses dari GUI thread)
// ============================================================================
static uint16_t s_tick_counter;
static bool s_popup_shown;
static bool s_dialog_shown;

static void net_scan_set_state(WlanApp* app, NetScanState state);

// ============================================================================
// OVERLAY MANAGEMENT
// ============================================================================
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

    // FIX: popup_reset selalu dipanggil (kalau s_popup_shown false,
    // net_scan_close_overlays tidak mereset). Ini memastikan state popup
    // selalu bersih sebelum di-set ulang.
    popup_reset(app->popup);

    popup_set_header(app->popup, "Network Scan", 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, text, 64, 32, AlignCenter, AlignCenter);
    popup_set_context(app->popup, app);
    popup_disable_timeout(app->popup);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPopup);
    s_popup_shown = true;
}

// ============================================================================
// RETRY DIALOG
// ============================================================================
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

// ============================================================================
// STATE MACHINE
// ============================================================================
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

// ============================================================================
// PUBLISH DEVICES FROM NETSCAN BUFFER TO APP
// FIX: gunakan counter terpisah (app->device_count) sebagai index tujuan,
// supaya kalau ada host yang di-skip (misal get_host gagal), tidak ada
// "slot kosong" di app->devices yang berisi data lama.
// ============================================================================
static void net_scan_publish_devices(WlanApp* app) {
    uint8_t count = wlan_netscan_get_host_count();
    if(count > WLAN_APP_MAX_DEVICES) count = WLAN_APP_MAX_DEVICES;

    app->device_count = 0;

    for(uint8_t i = 0; i < count; ++i) {
        WlanNetscanHost h;
        if(!wlan_netscan_get_host(i, &h)) continue;

        // Index tujuan berdasarkan jumlah device valid yang sudah disalin
        WlanDeviceRecord* d = &app->devices[app->device_count++];
        memset(d, 0, sizeof(*d));
        d->ip = h.ip;
        memcpy(d->mac, h.mac, 6);
        if(h.hostname[0]) {
            strncpy(d->hostname, h.hostname, sizeof(d->hostname) - 1);
            d->hostname[sizeof(d->hostname) - 1] = '\0';
        }
    }
}

// ============================================================================
// SCENE: on_enter
// ============================================================================
void wlan_app_scene_network_scanning_on_enter(void* context) {
    WlanApp* app = context;
    s_tick_counter = 0;
    s_popup_shown = false;
    s_dialog_shown = false;
    app->device_count = 0;

    // Force rescan: user minta scan ulang dari menu LAN
    if(app->lan_force_rescan) {
        app->lan_force_rescan = false;
        net_scan_set_state(app, NetScanStateArp);
        return;
    }

    // Coba load dari cache dulu
    uint16_t cached = 0;
    if(wlan_lan_cache_load(
           app->connected_ap.ssid, app->devices, WLAN_APP_MAX_DEVICES, &cached) &&
       cached > 0) {
        app->device_count = cached;

        // FIX: switch ke Loading dulu supaya tidak flash view scene sebelumnya
        // selama custom event di-queue.
        view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewLoading);

        view_dispatcher_send_custom_event(
            app->view_dispatcher, WlanAppCustomEventNetworkScanComplete);
        return;
    }

    // Cache miss: mulai scan ARP
    net_scan_set_state(app, NetScanStateArp);
}

// ============================================================================
// SCENE: on_event
// ============================================================================
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

                // FIX: cek app->device_count (bukan host_count) supaya konsisten
                // dengan data yang benar-benar tersalin.
                if(app->device_count == 0) {
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
                wlan_lan_cache_save(
                    app->connected_ap.ssid, app->devices, app->device_count);
                view_dispatcher_send_custom_event(
                    app->view_dispatcher, WlanAppCustomEventNetworkScanComplete);
            }
        }
    }

    return consumed;
}

// ============================================================================
// SCENE: on_exit
// ============================================================================
void wlan_app_scene_network_scanning_on_exit(void* context) {
    WlanApp* app = context;
    wlan_netscan_stop_hostname_resolve();
    net_scan_close_overlays(app);
    s_tick_counter = 0;
    scene_manager_set_scene_state(
        app->scene_manager, WlanAppSceneNetworkScanning, NetScanStateArp);
}
