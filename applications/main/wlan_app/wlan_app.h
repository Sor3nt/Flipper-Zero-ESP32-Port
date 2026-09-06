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
#include "wlan_mitm_payloads.h"
#include "wlan_evil_portal_templates.h"
#include "wlan_sd_update.h"
#include "wlan_fw_update.h"
#include "wlan_webfs.h"
#include "wlan_smb.h"
#include "wlan_androidtv.h"
#include "views/wlan_lan_view.h"
#include "views/wlan_androidtv_remote_view.h"
#include "views/wlan_connect_view.h"
#include "views/wlan_portscan_view.h"
#include "views/wlan_handshake_view.h"
#include "views/wlan_handshake_channel_view.h"
#include "views/wlan_deauther_view.h"
#include "views/wlan_smart_deauth_view.h"
#include "views/wlan_sniffer_view.h"
#include "views/wlan_evil_portal_view.h"
#include "views/wlan_evil_portal_captured_view.h"
#include "views/wlan_live_creds_view.h"
#include "views/wlan_sd_update_view.h"
#include "views/wlan_fw_update_view.h"

#define WLAN_APP_TAG "WlanApp"
#define WLAN_APP_MAX_APS 64
#define WLAN_APP_MAX_DEVICES 64
#define WLAN_APP_SSID_MAX 33
#define WLAN_APP_PASSWORD_MAX 65
#define WLAN_APP_HOSTNAME_MAX 32
#define WLAN_AIRSNITCH_MAX_PEERS 48

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
    WlanAppViewSmartDeauth,
    WlanAppViewSniffer,
    WlanAppViewEvilPortal,
    WlanAppViewEvilPortalCaptured,
    WlanAppViewLiveCreds,
    WlanAppViewSdUpdate,
    WlanAppViewFwUpdate,
    WlanAppViewAndroidTvRemote,
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
    bool sniff_monitor;     // ARP-MITM + transparenter Forward (Live Creds); exklusiv mit block
} WlanDeviceRecord;

// AirSnitch: ein vom Gastnetz aus erreichbares Gerät (Client-Isolation-Bypass).
// mac ist nur für same_subnet-Peers (L2/ARP) bekannt, sonst 0.
typedef struct {
    uint32_t ip;            // Network-Byte-Order
    uint8_t mac[6];         // 0 wenn Fremd-Subnetz (L3)
    char hostname[WLAN_APP_HOSTNAME_MAX];
    bool same_subnet;       // true = eigenes Gast-/24 (L2), false = Fremd-Subnetz (L3)
} WlanAirsnitchPeer;

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
    View* view_smart_deauth;
    View* view_sniffer;
    WlanSnifferView* sniffer_view_obj;
    View* view_evil_portal;
    WlanEvilPortalView* evil_portal_view_obj;
    View* view_evil_portal_captured;
    WlanEvilPortalCapturedView* evil_portal_captured_view_obj;
    View* view_live_creds;
    WlanLiveCredsView* live_creds_view_obj;
    WlanCredSniff* cred_sniff;

    // MiTM (vormals Live Creds) Settings — gesetzt in scene_mitm_menu, gelesen
    // in scene_live_creds (Run-Scene).
    bool mitm_inject_enabled;
    bool mitm_store_cred;
    // Bytes-grenze ist auch text-input-Limit für custom JS sowie Lade-Limit für
    // SD-Payloads in scene_live_creds (sizeof(...)). Muss zur USER_PAYLOAD_MAX
    // im wlan_html_inject passen, sonst wird der payload abgeschnitten.
    char mitm_inject_code[1024];
    // Payload-Auswahl: 0..mitm_payloads.count-1 = SD-File, == count = "custom"
    // (verwendet mitm_inject_code aus dem Text-Input). Liste wird in
    // scene_mitm_menu beim Enter neu gescannt.
    WlanMitmPayloadList mitm_payloads;
    uint8_t mitm_payload_index;

    // Evil Portal Settings/Captured State
    char evil_portal_ssid[33];
    uint8_t evil_portal_channel;
    // 0 = Builtin Google, 1 = Builtin Router,
    // >=2 = SD-Template evil_portal_templates.items[index-2].
    // Liste wird in scene_evil_portal_menu beim Enter neu gescannt.
    uint8_t evil_portal_template_index;
    WlanEvilPortalTemplateList evil_portal_templates;
    bool evil_portal_karma;             // Karma-Modus aktiv
    char evil_portal_valid_ssid[33];
    char evil_portal_valid_pwd[65];

    // Bridge Mode: after creds captured, switch to APSTA and forward client
    // traffic to an upstream real WiFi network so the victim gets real
    // internet. Replaces the static "Couldn't sign you in" page with a
    // delayed redirect to google.com when enabled.
    bool evil_portal_bridge_enable;
    char evil_portal_bridge_ssid[33];
    char evil_portal_bridge_password[65];

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
    bool lan_force_rescan;        // true → SD-Cache überspringen, echten ARP-Scan erzwingen

    // AirSnitch (Client-Isolation-Test vom Gastnetz aus)
    char airsnitch_target_ssid[WLAN_APP_SSID_MAX]; // reines Label des gewählten Zielnetzes
    WlanAirsnitchPeer airsnitch_peers[WLAN_AIRSNITCH_MAX_PEERS];
    uint8_t airsnitch_peer_count;

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

    // SD-Content-Update (Delta-Sync über files.txt). Worker + View werden von
    // der kombinierten Update-Scene (scene_fw_update) als zweite Phase genutzt.
    WlanSdUpdate* sd_update;
    View* view_sd_update;

    // Kombiniertes Update (Firmware + SD): true sobald der User "Update" gewählt
    // hat; scene_ssid_connect routet nach erfolgreichem Connect zur Update-Scene.
    // Wird in scene_fw_update konsumiert.
    bool fw_update_flow;
    WlanFwUpdate* fw_update;
    View* view_fw_update;

    // Web-Filesystem: like fw_update_flow, but routes to the Web-FS info scene
    // after a successful connect. webfs_ssid/pw hold the Dedicated-AP config.
    bool webfs_flow;
    char webfs_ssid[WLAN_WEBFS_SSID_MAX + 1];
    char webfs_pw[WLAN_WEBFS_PW_MAX + 1];

    // SMB Browser (only shown when connected). smb is lazily allocated on
    // first use and freed in wlan_app_free.
    WlanSmb* smb;
    char smb_server_ip[64];                 // selected server ("a.b.c.d")
    char smb_server_name[WLAN_APP_HOSTNAME_MAX]; // NetBIOS/host label
    char smb_user[WLAN_SMB_USER_MAX];
    char smb_pass[WLAN_SMB_PASS_MAX];
    char smb_share[WLAN_SMB_SHARE_MAX];     // current share, "" = share list level
    char smb_path[WLAN_SMB_PATH_MAX];       // current path within the share
    // Pending download target (set from the browser's long-OK menu).
    char smb_dl_share[WLAN_SMB_SHARE_MAX];
    char smb_dl_path[WLAN_SMB_PATH_MAX];
    char smb_dl_name[WLAN_SMB_NAME_MAX];
    bool smb_dl_is_dir;

    // Android TV remote (only shown when connected). androidtv is lazily
    // allocated on first use and freed in wlan_app_free.
    WlanAndroidTv* androidtv;
    View* view_androidtv_remote;
    char androidtv_ip[64]; // selected TV ("a.b.c.d")
    char androidtv_name[WLAN_ATV_NAME_MAX]; // host/NetBIOS label from the scan
    char androidtv_pin[8]; // 6-hex PIN entered during pairing
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
