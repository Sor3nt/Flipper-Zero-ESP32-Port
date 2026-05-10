#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <gui/modules/loading.h>
#include <gui/modules/popup.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>

#include "scenes/scenes.h"
#include "views/wlan_view_events.h"
#include "wlan_handshake_settings.h"
#include "views/wlan_lan_view.h"
#include "views/wlan_connect_view.h"
#include "views/wlan_portscan_view.h"
#include "views/wlan_handshake_view.h"
#include "views/wlan_handshake_channel_view.h"
#include "views/wlan_deauther_view.h"
#include "views/wlan_sniffer_view.h"
#include "views/wlan_evil_portal_view.h"
#include "views/wlan_evil_portal_captured_view.h"

#define WLAN_APP_TAG "WlanApp"
#define WLAN_APP_MAX_APS 64
#define WLAN_APP_MAX_DEVICES 64
#define WLAN_APP_SSID_MAX 33
#define WLAN_APP_PASSWORD_MAX 65
#define WLAN_APP_HOSTNAME_MAX 32

typedef enum {
    WlanAppViewSubmenu,
    WlanAppViewWidget,
    WlanAppViewLoading,
    WlanAppViewPopup,
    WlanAppViewTextInput,
    WlanAppViewVariableItemList,
    WlanAppViewLan,
    WlanAppViewConnect,
    WlanAppViewPortscan,
    WlanAppViewHandshake,
    WlanAppViewHandshakeChannel,
    WlanAppViewDeauther,
    WlanAppViewSniffer,
    WlanAppViewEvilPortal,
    WlanAppViewEvilPortalCaptured,
} WlanAppView;

typedef struct {
    char ssid[WLAN_APP_SSID_MAX];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t authmode;
    bool is_open;
    bool has_password;
} WlanApRecord;

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
    char hostname[WLAN_APP_HOSTNAME_MAX];
    bool active;            // VIP=false (default) / TARGET=true
    bool block_internet;    // exklusiv mit throttle_kbps
    uint16_t throttle_kbps; // 0 = aus
} WlanDeviceRecord;

#define WLAN_APP_MAX_DEAUTH_CLIENTS 16

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    bool cut; // false = VIP (default), true = CUT/SNIFF (mode-abhängig)
} WlanDeauthClient;

typedef enum {
    WlanAppThrottleOff = 0,
    WlanAppThrottle16,
    WlanAppThrottle32,
    WlanAppThrottle64,
    WlanAppThrottle128,
    WlanAppThrottle256,
    WlanAppThrottle512,
    WlanAppThrottle1024,
    WlanAppThrottleCount,
} WlanAppThrottleLevel;

typedef struct WlanNetcut WlanNetcut;

#define WLAN_APP_EVIL_PORTAL_QUEUE_SIZE 8
typedef struct {
    char user[64];
    char pwd[64];
} WlanAppEvilPortalCred;

typedef struct WlanApp WlanApp;

struct WlanApp {
    Gui* gui;
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Widget* widget;
    Loading* loading;
    Popup* popup;
    TextInput* text_input;
    VariableItemList* variable_item_list;
    View* view_lan;
    View* view_connect;
    View* view_portscan;
    View* view_handshake;
    View* view_handshake_channel;
    View* view_deauther;
    View* view_sniffer;
    WlanSnifferView* sniffer_view_obj;
    View* view_evil_portal;
    WlanEvilPortalView* evil_portal_view_obj;
    View* view_evil_portal_captured;
    WlanEvilPortalCapturedView* evil_portal_captured_view_obj;

    // Evil Portal Settings/Captured State
    char evil_portal_ssid[33];
    uint8_t evil_portal_channel;
    uint8_t evil_portal_template_index; // 0 = Google, 1 = Router
    char evil_portal_valid_ssid[33];
    char evil_portal_valid_pwd[65];

    // Lock-free Cred-Ring vom Evil-Portal-Callback gefüllt, von der Scene
    // im Tick gelesen.
    WlanAppEvilPortalCred evil_portal_cred_queue[WLAN_APP_EVIL_PORTAL_QUEUE_SIZE];
    volatile uint8_t evil_portal_cred_head;
    volatile uint8_t evil_portal_cred_tail;
    uint32_t evil_portal_cred_total;
    char evil_portal_last_user[64];

    // Scan-Resultate (Connect-Scene)
    WlanApRecord* ap_records;
    uint16_t ap_count;
    size_t ap_selected_index;

    // Connection / Target State
    bool connected;
    bool target_selected;
    WlanApRecord connected_ap;
    WlanApRecord target_ap;
    char password_input[WLAN_APP_PASSWORD_MAX];

    // Network Scan (LAN)
    WlanDeviceRecord* devices;
    uint16_t device_count;
    size_t device_selected_index;
    int16_t lan_menu_device_idx; // -1 = kein Menü aktiv, sonst Device-Index
    bool lan_popup_active;        // true wenn LAN-Scene gerade einen Popup overlayt
    bool lan_scan_complete;       // true wenn ARP-Scan einmal durchgelaufen ist

    // Deauther-/Sniffer-Picker-Scene-State (shared)
    WlanDeauthClient deauth_clients[WLAN_APP_MAX_DEAUTH_CLIENTS];
    uint8_t deauth_client_count;
    bool deauth_auto;             // Auto-Mode in Deauther-Scene
    char picker_associated_ssid[WLAN_APP_SSID_MAX]; // SSID/Channel-Key der Picker-Liste

    // Attack-Targets-Settings (live)
    bool attack_block_internet;
    WlanAppThrottleLevel attack_throttle;

    // Channel-Mode (Capture Handshake / Deauth / Sniffer aus Hauptmenü)
    bool channel_mode_active;     // true → Scenes ignorieren target_ap, arbeiten auf Channel-Ebene
    uint8_t channel_action_channel; // 1..13, Default 1; in der Scene umschaltbar
    uint8_t hs_channel_pending;     // Vorgeschlagener Channel im Confirm-Dialog

    // Handshake-Capture-Stats (echte Werte gesetzt durch scene_handshake)
    bool handshake_running;
    bool handshake_complete;
    bool handshake_deauth_running;
    uint32_t handshake_eapol_count;
    uint32_t handshake_deauth_count;

    // Persistente Handshake-Settings (Channel/Hopping/SaveTo).
    WlanHandshakeSettings hs_settings;

    // Beacon-Spam (SSID Spam) — persistiert nur scene-lokal.
    uint8_t beacon_mode; // WlanHalBeaconMode
    char beacon_custom_ssid[33];

    FuriString* text_buf;

    WlanNetcut* netcut;
};

/** Schlüssel der aktuellen Picker-Assoziation: Channel-Key im Channel-Mode,
 *  sonst SSID des Targets/Connected-AP (oder leerer String). */
static inline void wlan_app_picker_current_key(const WlanApp* app, char* out, size_t sz) {
    if(sz == 0) return;
    if(app->channel_mode_active) {
        snprintf(out, sz, "_ch_%u", (unsigned)app->channel_action_channel);
    } else {
        const char* k = app->target_ap.ssid[0] ? app->target_ap.ssid :
                        (app->connected ? app->connected_ap.ssid : "");
        strncpy(out, k, sz - 1);
        out[sz - 1] = '\0';
    }
}

/** True wenn die Picker-Liste zum aktuellen Target/Channel gehört. Wird false
 *  sobald der User Target/Channel wechselt, ohne den Picker zu öffnen — die
 *  Selektionen sind dann stale und sollten ignoriert werden. */
static inline bool wlan_app_picker_matches_current(const WlanApp* app) {
    char key[WLAN_APP_SSID_MAX];
    wlan_app_picker_current_key(app, key, sizeof(key));
    return strncmp(app->picker_associated_ssid, key, sizeof(app->picker_associated_ssid)) == 0;
}

static inline bool wlan_app_picker_has_selection(const WlanApp* app) {
    if(!wlan_app_picker_matches_current(app)) return false;
    for(uint8_t i = 0; i < app->deauth_client_count; ++i) {
        if(app->deauth_clients[i].cut) return true;
    }
    return false;
}

static inline uint8_t wlan_app_picker_selection_count(const WlanApp* app) {
    if(!wlan_app_picker_matches_current(app)) return 0;
    uint8_t count = 0;
    for(uint8_t i = 0; i < app->deauth_client_count; ++i) {
        if(app->deauth_clients[i].cut) count++;
    }
    return count;
}
