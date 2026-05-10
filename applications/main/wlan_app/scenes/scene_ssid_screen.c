#include "../wlan_app.h"
#include <assets_icons.h>

static void ssid_screen_select_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeLeft) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventSsidSelect);
    }
}

static void ssid_screen_connect_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventSsidConnect);
    }
}

void wlan_app_scene_ssid_screen_on_enter(void* context) {
    WlanApp* app = context;

    WlanApRecord* ap = NULL;
    if(app->ap_selected_index < app->ap_count) {
        ap = &app->ap_records[app->ap_selected_index];
    }

    if(!ap) {
        widget_add_string_element(
            app->widget, 64, 32, AlignCenter, AlignCenter, FontPrimary, "No AP");
        view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
        return;
    }

    // Header: SSID links, RSSI rechts, beide Baseline y=10.
    widget_add_string_element(
        app->widget, 2, 10, AlignLeft, AlignBottom, FontPrimary, ap->ssid);

    char rssi_buf[16];
    snprintf(rssi_buf, sizeof(rssi_buf), "%d dBm", ap->rssi);
    widget_add_string_element(
        app->widget, 126, 10, AlignRight, AlignBottom, FontSecondary, rssi_buf);

    // Trennlinie unter dem Header.
    widget_add_line_element(app->widget, 0, 13, 127, 13);

    // MAC mittig zentriert.
    char mac[24];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
        ap->bssid[0], ap->bssid[1], ap->bssid[2],
        ap->bssid[3], ap->bssid[4], ap->bssid[5]);
    widget_add_string_element(
        app->widget, 64, 28, AlignCenter, AlignBottom, FontSecondary, mac);

    // Channel links + Lock/Unlock-Icon rechts in derselben Zeile.
    char ch_buf[16];
    snprintf(ch_buf, sizeof(ch_buf), "Channel: %u", (unsigned)ap->channel);
    widget_add_string_element(
        app->widget, 2, 44, AlignLeft, AlignBottom, FontSecondary, ch_buf);

    bool unlocked = ap->is_open || ap->has_password;
    const Icon* lock = unlocked ? &I_Unlock_7x8 : &I_Lock_7x8;
    widget_add_icon_element(app->widget, 128 - 7 - 4, 36, lock);

    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "Attack", ssid_screen_select_cb, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeRight, "Connect", ssid_screen_connect_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

bool wlan_app_scene_ssid_screen_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(app->ap_selected_index >= app->ap_count) return false;
        WlanApRecord* ap = &app->ap_records[app->ap_selected_index];

        if(event.event == WlanAppCustomEventSsidSelect) {
            memcpy(&app->target_ap, ap, sizeof(WlanApRecord));
            app->target_selected = true;
            scene_manager_next_scene(app->scene_manager, WlanAppSceneNetworkActions);
            consumed = true;
        } else if(event.event == WlanAppCustomEventSsidConnect) {
            memcpy(&app->target_ap, ap, sizeof(WlanApRecord));
            scene_manager_next_scene(app->scene_manager, WlanAppSceneSsidConnect);
            consumed = true;
        }
    }

    return consumed;
}

void wlan_app_scene_ssid_screen_on_exit(void* context) {
    WlanApp* app = context;
    widget_reset(app->widget);
}
