#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <gui/modules/loading.h>
#include <gui/modules/text_input.h>

#include "scenes/scenes.h"
#include "wifi_crawler.h"

#define WIFI_APP_MAX_APS 64
#define WIFI_APP_LOG_TAG "WifiApp"

/** Deauth mode */
typedef enum {
    WifiAppDeauthModeSsid,    // Target specific connected AP
    WifiAppDeauthModeChannel, // All APs on channel
} WifiAppDeauthMode;

/** Custom events */
typedef enum {
    WifiAppCustomEventScanComplete = 100,
    WifiAppCustomEventApSelected,
    WifiAppCustomEventDeauthToggle,
    WifiAppCustomEventConnect,
    WifiAppCustomEventCrawlerDomainEntered,
    WifiAppCustomEventCrawlerStop,
    WifiAppCustomEventHandshakeToggle,
    WifiAppCustomEventHandshakeDeauth,
    WifiAppCustomEventPasswordEntered,
} WifiAppCustomEvent;

/** View IDs */
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
} WifiAppView;

/** Per-AP scan result */
typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t authmode;
    bool is_open;       // authmode == WIFI_AUTH_OPEN
    bool has_password;  // password file on SD card
} WifiApRecord;

/** Main app struct */
typedef struct {
    Gui* gui;
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;

    // GUI modules
    Submenu* submenu;
    Widget* widget;
    Loading* loading;
    TextInput* text_input;
    View* view_ap_list;
    View* view_deauther;
    View* view_sniffer;
    View* view_crawler;
    View* view_handshake;
    View* view_handshake_channel;
    View* view_airsnitch;

    // Scan results
    WifiApRecord* ap_records;
    uint16_t ap_count;
    size_t selected_index;
    FuriString* text_buf;

    // Deauther state
    volatile bool deauth_running;
    uint32_t deauth_frame_count;

    // Sniffer state
    volatile bool sniffer_running;
    uint32_t sniffer_pkt_count;
    uint32_t sniffer_bytes;
    uint8_t sniffer_channel;

    // Handshake capture state
    volatile bool handshake_running;
    volatile bool handshake_deauth_running;
    uint32_t handshake_eapol_count;
    uint32_t handshake_deauth_count;
    bool handshake_complete;

    // Scanner navigation: where to go after AP selection
    uint8_t scanner_next_scene;

    // Connected AP info (stored after successful connect)
    WifiApRecord connected_ap;

    // Deauth mode
    WifiAppDeauthMode deauth_mode;

    // Password input buffer
    char password_input[65];

    // Crawler state
    char crawler_domain[128];
    WifiCrawlerState crawler_state;

    // Network scanner state
    View* view_netscan;
    uint32_t portscan_target_ip;
} WifiApp;

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
