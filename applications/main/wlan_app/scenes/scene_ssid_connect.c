#include "../wlan_app.h"
#include "../wlan_hal.h"
#include "../wlan_passwords.h"
#include "../wlan_netcut.h"

typedef enum {
    SsidConnectStateAskPassword = 0,
    SsidConnectStateConnecting,
    SsidConnectStateConnected,
    SsidConnectStateFailed,
} SsidConnectState;

#define SSID_CONNECT_TIMEOUT_TICKS 40   // 40 * 250 ms ≈ 10 s
#define SSID_CONNECT_POPUP_MS 1000

static uint16_t s_connect_ticks;

static void ssid_connect_set_state(WlanApp* app, SsidConnectState state);

static void ssid_connect_password_cb(void* context) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventPasswordEntered);
}

static void ssid_connect_popup_cb(void* context) {
    WlanApp* app = context;
    SsidConnectState s = (SsidConnectState)scene_manager_get_scene_state(
        app->scene_manager, WlanAppSceneSsidConnect);

    if(s == SsidConnectStateConnected) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventConnectSuccess);
    } else if(s == SsidConnectStateFailed) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventConnectFailed);
    }
}

static void ssid_connect_show_popup(WlanApp* app, const char* text, uint32_t timeout_ms) {
    popup_reset(app->popup);
    popup_set_header(app->popup, "WiFi", 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, text, 64, 32, AlignCenter, AlignCenter);
    popup_set_context(app->popup, app);
    popup_set_callback(app->popup, ssid_connect_popup_cb);
    if(timeout_ms > 0) {
        popup_set_timeout(app->popup, timeout_ms);
        popup_enable_timeout(app->popup);
    } else {
        popup_disable_timeout(app->popup);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPopup);
}

static void ssid_connect_set_state(WlanApp* app, SsidConnectState state) {
    scene_manager_set_scene_state(app->scene_manager, WlanAppSceneSsidConnect, state);

    switch(state) {
    case SsidConnectStateAskPassword:
        text_input_reset(app->text_input);
        app->password_input[0] = '\0';
        text_input_set_header_text(app->text_input, "WiFi Password:");
        text_input_set_result_callback(
            app->text_input,
            ssid_connect_password_cb,
            app,
            app->password_input,
            sizeof(app->password_input),
            true);
        view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewTextInput);
        break;

    case SsidConnectStateConnecting:
        s_connect_ticks = 0;
        if(!wlan_hal_is_started()) wlan_hal_start();
        wlan_hal_connect(
            app->target_ap.ssid,
            app->password_input,
            app->target_ap.bssid,
            app->target_ap.channel);
        ssid_connect_show_popup(app, "Connecting ...", 0);
        break;

    case SsidConnectStateConnected:
        ssid_connect_show_popup(app, "Connected!", SSID_CONNECT_POPUP_MS);
        break;

    case SsidConnectStateFailed:
        ssid_connect_show_popup(app, "Failed!", SSID_CONNECT_POPUP_MS);
        break;
    }
}

void wlan_app_scene_ssid_connect_on_enter(void* context) {
    WlanApp* app = context;
    s_connect_ticks = 0;

    // Wenn ein Passwort bereits auf der SD existiert, lade es vor dem Connect.
    if(!app->target_ap.is_open && app->target_ap.has_password) {
        wlan_password_read(
            app->target_ap.ssid, app->password_input, sizeof(app->password_input));
    } else if(app->target_ap.is_open) {
        app->password_input[0] = '\0';
    }

    bool need_password = !app->target_ap.is_open && !app->target_ap.has_password;
    ssid_connect_set_state(
        app, need_password ? SsidConnectStateAskPassword : SsidConnectStateConnecting);
}

bool wlan_app_scene_ssid_connect_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventPasswordEntered) {
            // Speichere Passwort in /ext/wifi/<ssid>.txt für späteres Auto-Connect.
            if(app->password_input[0]) {
                wlan_password_save(app->target_ap.ssid, app->password_input);
            }
            app->target_ap.has_password = true;
            ssid_connect_set_state(app, SsidConnectStateConnecting);
            consumed = true;
        } else if(event.event == WlanAppCustomEventConnectSuccess) {
            memcpy(&app->connected_ap, &app->target_ap, sizeof(WlanApRecord));
            app->connected = true;
            app->target_selected = false;
            app->lan_scan_complete = false;
            // gw_mac aus DHCP-Antwort sollte jetzt in der ARP-Tabelle stehen.
            wlan_netcut_preflight(app->netcut);
            scene_manager_next_scene(app->scene_manager, WlanAppSceneNetworkScanning);
            consumed = true;
        } else if(event.event == WlanAppCustomEventConnectFailed) {
            // Bei Failed: gespeichertes Passwort verwerfen damit der User es erneut eingibt.
            app->target_ap.has_password = false;
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        SsidConnectState s = (SsidConnectState)scene_manager_get_scene_state(
            app->scene_manager, WlanAppSceneSsidConnect);
        if(s == SsidConnectStateConnecting) {
            if(wlan_hal_is_connected()) {
                ssid_connect_set_state(app, SsidConnectStateConnected);
            } else if(++s_connect_ticks >= SSID_CONNECT_TIMEOUT_TICKS) {
                wlan_hal_disconnect();
                ssid_connect_set_state(app, SsidConnectStateFailed);
            }
        }
    }

    return consumed;
}

void wlan_app_scene_ssid_connect_on_exit(void* context) {
    WlanApp* app = context;
    popup_reset(app->popup);
    text_input_reset(app->text_input);
    s_connect_ticks = 0;
    scene_manager_set_scene_state(
        app->scene_manager, WlanAppSceneSsidConnect, SsidConnectStateAskPassword);
}
