#include "../streaming.h"

#include <wlan_hal.h>
#include <wlan_passwords.h>
#include <wifi.h>
#include <string.h>

typedef enum {
    ConnStateAskPassword = 0,
    ConnStateConnecting,
    ConnStateConnected,
    ConnStateFailed,
} ConnState;

#define CONNECT_TIMEOUT_TICKS 40 /* 40 * 250 ms ≈ 10 s */
#define CONNECT_POPUP_MS      1000

static uint16_t s_ticks;

static void conn_set_state(StreamingApp* app, ConnState state);

static void password_cb(void* ctx) {
    StreamingApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, StreamingEventPasswordEntered);
}

static void popup_cb(void* ctx) {
    StreamingApp* app = ctx;
    ConnState s =
        (ConnState)scene_manager_get_scene_state(app->scene_manager, StreamingSceneWifiConnect);
    if(s == ConnStateConnected) {
        view_dispatcher_send_custom_event(app->view_dispatcher, StreamingEventConnectSuccess);
    } else if(s == ConnStateFailed) {
        view_dispatcher_send_custom_event(app->view_dispatcher, StreamingEventConnectFailed);
    }
}

static void show_popup(StreamingApp* app, const char* text, uint32_t timeout_ms) {
    popup_reset(app->popup);
    popup_set_header(app->popup, "WiFi", 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, text, 64, 32, AlignCenter, AlignCenter);
    popup_set_context(app->popup, app);
    popup_set_callback(app->popup, popup_cb);
    if(timeout_ms > 0) {
        popup_set_timeout(app->popup, timeout_ms);
        popup_enable_timeout(app->popup);
    } else {
        popup_disable_timeout(app->popup);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, StreamingViewPopup);
}

static void conn_set_state(StreamingApp* app, ConnState state) {
    scene_manager_set_scene_state(app->scene_manager, StreamingSceneWifiConnect, state);

    switch(state) {
    case ConnStateAskPassword:
        text_input_reset(app->text_input);
        app->password_input[0] = '\0';
        text_input_set_header_text(app->text_input, "WiFi Password:");
        text_input_set_result_callback(
            app->text_input,
            password_cb,
            app,
            app->password_input,
            sizeof(app->password_input),
            true);
        view_dispatcher_switch_to_view(app->view_dispatcher, StreamingViewTextInput);
        break;

    case ConnStateConnecting:
        s_ticks = 0;
        if(!wlan_hal_is_started()) wlan_hal_start();
        wlan_hal_connect(
            app->target_ap.ssid,
            app->password_input,
            app->target_ap.bssid,
            app->target_ap.channel);
        show_popup(app, "Connecting ...", 0);
        break;

    case ConnStateConnected:
        show_popup(app, "Connected!", CONNECT_POPUP_MS);
        break;

    case ConnStateFailed:
        show_popup(app, "Failed!", CONNECT_POPUP_MS);
        break;
    }
}

void streaming_scene_wifi_connect_on_enter(void* context) {
    StreamingApp* app = context;
    s_ticks = 0;

    if(!app->target_ap.is_open && app->target_ap.has_password) {
        wlan_password_read(app->target_ap.ssid, app->password_input, sizeof(app->password_input));
    } else if(app->target_ap.is_open) {
        app->password_input[0] = '\0';
    }

    bool need_password = !app->target_ap.is_open && !app->target_ap.has_password;
    conn_set_state(app, need_password ? ConnStateAskPassword : ConnStateConnecting);
}

bool streaming_scene_wifi_connect_on_event(void* context, SceneManagerEvent event) {
    StreamingApp* app = context;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == StreamingEventPasswordEntered) {
            if(app->password_input[0]) {
                wlan_password_save(app->target_ap.ssid, app->password_input);
            }
            app->target_ap.has_password = true;
            conn_set_state(app, ConnStateConnecting);
            return true;
        }
        if(event.event == StreamingEventConnectSuccess) {
            /* Make the connection global/sticky (survives leaving the app). */
            wifi_mark_connected((Wifi*)app->wifi, app->target_ap.ssid);
            scene_manager_next_scene(app->scene_manager, StreamingSceneDeviceScan);
            return true;
        }
        if(event.event == StreamingEventConnectFailed) {
            if(!app->target_ap.is_open && wlan_hal_last_fail_is_auth()) {
                wlan_password_delete(app->target_ap.ssid);
                app->target_ap.has_password = false;
                if(app->ap_selected_index < app->ap_count) {
                    app->ap_records[app->ap_selected_index].has_password = false;
                }
            }
            scene_manager_previous_scene(app->scene_manager);
            return true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        ConnState s = (ConnState)scene_manager_get_scene_state(
            app->scene_manager, StreamingSceneWifiConnect);
        if(s == ConnStateConnecting) {
            if(wlan_hal_is_connected()) {
                conn_set_state(app, ConnStateConnected);
            } else if(++s_ticks >= CONNECT_TIMEOUT_TICKS) {
                wlan_hal_disconnect();
                conn_set_state(app, ConnStateFailed);
            }
        }
    }

    return false;
}

void streaming_scene_wifi_connect_on_exit(void* context) {
    StreamingApp* app = context;
    popup_reset(app->popup);
    text_input_reset(app->text_input);
    s_ticks = 0;
    scene_manager_set_scene_state(
        app->scene_manager, StreamingSceneWifiConnect, ConnStateAskPassword);
}
