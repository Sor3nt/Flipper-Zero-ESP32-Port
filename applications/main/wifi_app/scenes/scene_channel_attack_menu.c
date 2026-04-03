#include "../wifi_app.h"

enum ChannelAttackIndex {
    ChannelAttackIndexHandshake,
    ChannelAttackIndexDeauth,
    ChannelAttackIndexSniffer,
};

static void channel_attack_callback(void* context, uint32_t index) {
    WifiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wifi_app_scene_channel_attack_menu_on_enter(void* context) {
    WifiApp* app = context;

    submenu_add_item(
        app->submenu, "Capture Handshake", ChannelAttackIndexHandshake,
        channel_attack_callback, app);
    submenu_add_item(
        app->submenu, "Deauth", ChannelAttackIndexDeauth,
        channel_attack_callback, app);
    submenu_add_item(
        app->submenu, "Sniffer", ChannelAttackIndexSniffer,
        channel_attack_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewSubmenu);
}

bool wifi_app_scene_channel_attack_menu_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case ChannelAttackIndexHandshake:
            scene_manager_next_scene(app->scene_manager, WifiAppSceneHandshakeChannel);
            consumed = true;
            break;
        case ChannelAttackIndexDeauth:
            app->deauth_mode = WifiAppDeauthModeChannel;
            scene_manager_next_scene(app->scene_manager, WifiAppSceneDeauther);
            consumed = true;
            break;
        case ChannelAttackIndexSniffer:
            scene_manager_next_scene(app->scene_manager, WifiAppSceneSniffer);
            consumed = true;
            break;
        }
    }

    return consumed;
}

void wifi_app_scene_channel_attack_menu_on_exit(void* context) {
    WifiApp* app = context;
    submenu_reset(app->submenu);
}
