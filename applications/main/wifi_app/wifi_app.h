#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <gui/modules/loading.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>

#include "scenes/scenes.h"
#include "wifi_crawler.h"

#define WIFI_APP_MAX_APS 64
#define WIFI_APP_LOG_TAG "WifiApp"

typedef enum {
    WifiAppDeauthModeSsid,
    WifiAppDeauthModeChannel,
} WifiAppDeauthMode;

typedef enum {
    WifiAppCustomEventScanComplete = 100,
    WifiAppCustomEventApSelected,
    WifiAppCustomEventDeauthToggle,
    WifiAppCustomEventConnect,
    WifiAppCustomEventSelect,
    WifiAppCustomEventCrawlerDomainEntered,
    WifiAppCustomEventCrawlerStop,
    WifiAppCustomEventHandshakeToggle,
    WifiAppCustomEventHandshakeDeauth,
    WifiAppCustomEventPasswordEntered,
    WifiAppCustomEventBeaconStop,
    WifiAppCustomEventEvilPortalSsidEntered,
    WifiAppCustomEventEvilPortalCredCaptured,
    WifiAppCustomEventEvilPortalCredsValid,
    WifiAppCustomEventEvilPortalStop,
    WifiAppCustomEventEvilPortalConfig,
    WifiAppCustomEventEvilPortalTogglePause,
} WifiAppCustomEvent;

typedef enum {
    WifiAppViewSubmenu,
    WifiAppViewWidget,
    WifiAppViewLoading,
    WifiAppViewApList,
    WifiAppViewDeauther,
    WifiAppViewSniffer,
    WifiAppViewTextInput,
    WifiAppViewCrawler,
    WifiAppViewHandshake,
    WifiAppViewHandshakeChannel,
    WifiAppViewAirSnitch,
    WifiAppViewNetscan,
    WifiAppViewBeacon,
    WifiAppViewPortscan,
    WifiAppViewEvilPortal,
    WifiAppViewVariableItemList,
    WifiAppViewEvilPortalCaptured,
} WifiAppView;

typedef enum {
    WifiAppEvilPortalTemplateGoogle = 0,
    WifiAppEvilPortalTemplateRouter,
    WifiAppEvilPortalTemplateSd,
} WifiAppEvilPortalTemplate;

typedef struct {
    char user[64];
    char pwd[64];
} WifiAppEvilPortalCred;

#define WIFI_APP_EVIL_PORTAL_QUEUE_SIZE 16

typedef enum {
    WifiAppBeaconModeFunny,
    WifiAppBeaconModeRickroll,
    WifiAppBeaconModeRandom,
    WifiAppBeaconModeCustom,
} WifiAppBeaconMode;

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t authmode;
    bool is_open;
    bool has_password;
} WifiApRecord;

typedef struct WifiApp WifiApp;

struct WifiApp {
    Gui* gui;
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Widget* widget;
    Loading* loading;
    TextInput* text_input;
    VariableItemList* variable_item_list;
    View* view_ap_list;
    View* view_deauther;
    View* view_sniffer;
    View* view_crawler;
    View* view_handshake;
    View* view_handshake_channel;
    View* view_airsnitch;
    View* view_netscan;
    View* view_beacon;
    View* view_portscan;
    View* view_evil_portal;
    void* beacon_view_obj;
    void* evil_portal_view_obj;
    void* evil_portal_captured_view_obj;
    WifiApRecord* ap_records;
    uint16_t ap_count;
    size_t selected_index;
    FuriString* text_buf;
    volatile bool deauth_running;
    uint32_t deauth_frame_count;
    volatile bool sniffer_running;
    uint32_t sniffer_pkt_count;
    uint32_t sniffer_bytes;
    uint8_t sniffer_channel;
    volatile bool handshake_running;
    volatile bool handshake_deauth_running;
    uint32_t handshake_eapol_count;
    uint32_t handshake_deauth_count;
    bool handshake_complete;
    uint8_t scanner_next_scene;
    WifiApRecord connected_ap;
    bool ap_selected;
    WifiAppDeauthMode deauth_mode;
    char password_input[65];
    char crawler_domain[128];
    WifiCrawlerState crawler_state;
    uint32_t portscan_target_ip;
    WifiAppBeaconMode beacon_mode;
    char single_ssid[33];
    char evil_portal_ssid[33];
    uint8_t evil_portal_channel;
    WifiAppEvilPortalTemplate evil_portal_template;
    char evil_portal_sd_path[128];
    char evil_portal_valid_ssid[33];
    char evil_portal_valid_pwd[65];
    WifiAppEvilPortalCred evil_portal_cred_queue[WIFI_APP_EVIL_PORTAL_QUEUE_SIZE];
    volatile uint8_t evil_portal_cred_head;
    volatile uint8_t evil_portal_cred_tail;
    uint32_t evil_portal_cred_total;
};

static inline const char* wifi_auth_mode_str(int authmode) {
    switch(authmode) {
    case 0: return "OPEN";
    case 1: return "WEP";
    case 2: return "WPA";
    case 3: return "WPA2";
    case 4: return "WPA/2";
    case 6: return "WPA3";
    case 7: return "WPA2/3";
    default: return "?";
    }
}