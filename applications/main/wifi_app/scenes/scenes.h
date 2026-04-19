#pragma once

#include <gui/scene_manager.h>

#define ADD_SCENE(prefix, name, id) WifiAppScene##id,
typedef enum {
#include "scene_config.h"
    WifiAppSceneNum
} WifiAppScene;
#undef ADD_SCENE

void wifi_app_scene_menu_on_enter(void* context);
void wifi_app_scene_menu_on_exit(void* context);
bool wifi_app_scene_menu_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_scanner_on_enter(void* context);
void wifi_app_scene_scanner_on_exit(void* context);
bool wifi_app_scene_scanner_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_ap_detail_on_enter(void* context);
void wifi_app_scene_ap_detail_on_exit(void* context);
bool wifi_app_scene_ap_detail_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_deauther_on_enter(void* context);
void wifi_app_scene_deauther_on_exit(void* context);
bool wifi_app_scene_deauther_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_sniffer_on_enter(void* context);
void wifi_app_scene_sniffer_on_exit(void* context);
bool wifi_app_scene_sniffer_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_connect_on_enter(void* context);
void wifi_app_scene_connect_on_exit(void* context);
bool wifi_app_scene_connect_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_password_input_on_enter(void* context);
void wifi_app_scene_password_input_on_exit(void* context);
bool wifi_app_scene_password_input_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_crawler_input_on_enter(void* context);
void wifi_app_scene_crawler_input_on_exit(void* context);
bool wifi_app_scene_crawler_input_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_crawler_on_enter(void* context);
void wifi_app_scene_crawler_on_exit(void* context);
bool wifi_app_scene_crawler_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_handshake_on_enter(void* context);
void wifi_app_scene_handshake_on_exit(void* context);
bool wifi_app_scene_handshake_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_handshake_channel_on_enter(void* context);
void wifi_app_scene_handshake_channel_on_exit(void* context);
bool wifi_app_scene_handshake_channel_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_airsnitch_on_enter(void* context);
void wifi_app_scene_airsnitch_on_exit(void* context);
bool wifi_app_scene_airsnitch_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_airsn_scan_on_enter(void* context);
void wifi_app_scene_airsn_scan_on_exit(void* context);
bool wifi_app_scene_airsn_scan_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_netscan_on_enter(void* context);
void wifi_app_scene_netscan_on_exit(void* context);
bool wifi_app_scene_netscan_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_portscan_on_enter(void* context);
void wifi_app_scene_portscan_on_exit(void* context);
bool wifi_app_scene_portscan_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_channel_attack_menu_on_enter(void* context);
void wifi_app_scene_channel_attack_menu_on_exit(void* context);
bool wifi_app_scene_channel_attack_menu_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_ssid_attack_menu_on_enter(void* context);
void wifi_app_scene_ssid_attack_menu_on_exit(void* context);
bool wifi_app_scene_ssid_attack_menu_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_spam_ssids_menu_on_enter(void* context);
void wifi_app_scene_spam_ssids_menu_on_exit(void* context);
bool wifi_app_scene_spam_ssids_menu_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_beacon_spam_on_enter(void* context);
void wifi_app_scene_beacon_spam_on_exit(void* context);
bool wifi_app_scene_beacon_spam_on_event(void* context, SceneManagerEvent event);

void wifi_app_scene_custom_ssid_on_enter(void* context);
void wifi_app_scene_custom_ssid_on_exit(void* context);
bool wifi_app_scene_custom_ssid_on_event(void* context, SceneManagerEvent event);

extern const SceneManagerHandlers wifi_app_scene_handlers;

#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_enter(void*);
#include "scene_config.h"
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) bool prefix##_scene_##name##_on_event(void* context, SceneManagerEvent event);
#include "scene_config.h"
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_exit(void* context);
#include "scene_config.h"
#undef ADD_SCENE