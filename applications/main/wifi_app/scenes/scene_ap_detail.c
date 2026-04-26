#include "../wifi_app.h"
#include <string.h>

static void ap_detail_apply_evil_portal_target(WifiApp* app) {
    if(app->connected_ap.ssid[0] == '\0') return;
    strncpy(app->evil_portal_ssid, app->connected_ap.ssid, sizeof(app->evil_portal_ssid) - 1);
    app->evil_portal_ssid[sizeof(app->evil_portal_ssid) - 1] = '\0';
    if(app->connected_ap.channel >= 1 && app->connected_ap.channel <= 14) {
        app->evil_portal_channel = app->connected_ap.channel;
    }
}

static void __attribute__((unused)) ap_detail_deauth_callback(GuiButtonType result, InputType type, void* context) {
    WifiApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WifiAppCustomEventDeauthToggle);
    }
}

static void ap_detail_connect_callback(GuiButtonType result, InputType type, void* context) {
    WifiApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WifiAppCustomEventConnect);
    }
}

static void ap_detail_select_callback(GuiButtonType result, InputType type, void* context) {
    WifiApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeLeft) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WifiAppCustomEventSelect);
    }
}

void wifi_app_scene_ap_detail_on_enter(void* context) {
    WifiApp* app = context;

    if(app->selected_index < app->ap_count) {
        WifiApRecord* ap = &app->ap_records[app->selected_index];
        const char* ssid = ap->ssid[0] ? ap->ssid : "(hidden)";

        furi_string_printf(
            app->text_buf,
            "SSID: %s\n"
            "BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n"
            "RSSI: %d dBm\n"
            "Channel: %d\n"
            "Auth: %s",
            ssid,
            ap->bssid[0], ap->bssid[1], ap->bssid[2],
            ap->bssid[3], ap->bssid[4], ap->bssid[5],
            ap->rssi, ap->channel,
            wifi_auth_mode_str(ap->authmode));
    } else {
        furi_string_set_str(app->text_buf, "No AP data");
    }

    widget_add_string_multiline_element(
        app->widget, 0, 2, AlignLeft, AlignTop, FontSecondary,
        furi_string_get_cstr(app->text_buf));

    if(app->selected_index < app->ap_count) {
        widget_add_button_element(
            app->widget, GuiButtonTypeLeft, "Select",
            ap_detail_select_callback, app);
        widget_add_button_element(
            app->widget, GuiButtonTypeRight, "Connect",
            ap_detail_connect_callback, app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewWidget);
}

bool wifi_app_scene_ap_detail_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WifiAppCustomEventDeauthToggle) {
            scene_manager_next_scene(app->scene_manager, WifiAppSceneDeauther);
            consumed = true;
        } else if(event.event == WifiAppCustomEventConnect) {
            WifiApRecord* ap = &app->ap_records[app->selected_index];
            memcpy(&app->connected_ap, ap, sizeof(WifiApRecord));
            app->ap_selected = true;
            ap_detail_apply_evil_portal_target(app);
            if(ap->is_open || ap->has_password) {
                scene_manager_next_scene(app->scene_manager, WifiAppSceneConnect);
            } else {
                scene_manager_next_scene(app->scene_manager, WifiAppScenePasswordInput);
            }
            consumed = true;
        } else if(event.event == WifiAppCustomEventSelect) {
            if(app->selected_index < app->ap_count) {
                memcpy(&app->connected_ap, &app->ap_records[app->selected_index], sizeof(WifiApRecord));
                app->ap_selected = true;
                ap_detail_apply_evil_portal_target(app);
            }
            scene_manager_search_and_switch_to_previous_scene(app->scene_manager, WifiAppSceneMenu);
            consumed = true;
        }
    }

    return consumed;
}

void wifi_app_scene_ap_detail_on_exit(void* context) {
    WifiApp* app = context;
    widget_reset(app->widget);
}
