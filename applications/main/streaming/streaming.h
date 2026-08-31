#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/file_browser.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/popup.h>
#include <gui/modules/loading.h>
#include <esp_wifi.h>

#include "scenes/scenes.h"
#include "views/player_view.h"

#define STREAMING_TAG          "Streaming"
#define STREAMING_DATA_DIR     "/ext/apps_data/medien"
#define STREAMING_PATH_MAX     256
#define STREAMING_NAME_MAX     128
#define STREAMING_MAX_APS      48
#define STREAMING_MAX_DEVICES  24
#define STREAMING_SSID_MAX     33
#define STREAMING_PASSWORD_MAX 65

/* Media type of the selected file (drives the action menu + playback path). */
typedef enum {
    MediaKindAudio, /* .mp3 */
    MediaKindVideo, /* .mp4 */
} MediaKind;

/* Which discovery protocol found a streaming target — decides the sink. */
typedef enum {
    StreamDeviceAirplay, /* RAOP receiver (audio only) */
    StreamDeviceCast,    /* Google Cast (audio + video) */
    StreamDeviceDlna,    /* DLNA MediaRenderer (audio + video) */
} StreamDeviceType;

/* One discovered streaming target (AirPlay mDNS or DLNA/Cast SSDP result,
 * unified so the device-scan list can mix both). */
typedef struct {
    StreamDeviceType type;
    char name[48];
    uint32_t ip; /* network byte order */
    uint16_t port;
    /* DLNA / Cast */
    char av_control[128];
    char rc_control[128];
    bool has_cast;
    /* AirPlay */
    int et;
    int cn;
    bool needs_password;
} StreamDevice;

/* How the current media is being played out. */
typedef enum {
    PlayModeLocal,   /* audio → I2S speaker */
    PlayModeAirplay, /* audio → RAOP sender */
    PlayModeCast,    /* audio/video → Google Cast */
    PlayModeDlna,    /* audio/video → DLNA AVTransport */
} PlayMode;

typedef enum {
    PlaybackIdle,
    PlaybackPlaying,
    PlaybackPaused,
} PlaybackState;

/* Shared view-dispatcher view ids. */
typedef enum {
    StreamingViewFileBrowser,
    StreamingViewSubmenu,
    StreamingViewTextInput,
    StreamingViewPopup,
    StreamingViewLoading,
    StreamingViewPlayer,
    StreamingViewLan, /* shared LAN-list view (device scan), same look as wlan_app */
} StreamingView;

/* Custom events posted from the player view / worker threads to the scenes. */
typedef enum {
    StreamingEventPlayPause = 100,
    StreamingEventSeekBack,
    StreamingEventSeekFwd,
    StreamingEventNext,
    StreamingEventOpenConfig, /* reserved for later (settings) */
    StreamingEventWifiScanDone,
    StreamingEventDeviceScanDone,
    StreamingEventPasswordEntered,
    StreamingEventConnectSuccess,
    StreamingEventConnectFailed,
} StreamingCustomEvent;

/* One scanned access point. */
typedef struct {
    char ssid[STREAMING_SSID_MAX];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t authmode;
    bool is_open;
    bool has_password;
} StreamingApRecord;

typedef struct StreamingApp StreamingApp;

struct StreamingApp {
    Gui* gui;
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;
    void* wifi; /* Wifi* (RECORD_WIFI) */

    /* GUI modules */
    FileBrowser* file_browser;
    FuriString* browser_path; /* result path from the file browser */
    Submenu* submenu;
    TextInput* text_input;
    Popup* popup;
    Loading* loading;
    PlayerView* player_view;
    View* view_lan; /* wlan_app's shared LAN list view, reused for device scan */

    /* selected media */
    char sel_path[STREAMING_PATH_MAX];
    char sel_name[STREAMING_NAME_MAX];
    MediaKind sel_kind;

    /* message shown by scene_error (e.g. unsupported AirPlay encryption) */
    char error_msg[64];

    /* playback engine (see stream_player.c) */
    PlayMode play_mode;
    PlaybackState playback;
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    uint8_t volume;
    bool speaker_owned;       /* true while we hold the I2S speaker HAL */
    volatile bool audio_ended; /* set by the decoder EOF callback */

    /* DLNA local progress fallback: many renderers accept Play but have no
     * working GetPositionInfo. When the poll never returns a position, count
     * elapsed time locally from the play start instead of showing 0:00. */
    uint32_t dlna_play_tick;
    bool dlna_pos_from_soap;

    /* async SOAP command in flight (DLNA path) */
    volatile bool cmd_busy;
    FuriThread* cmd_thread;
    int pending_cmd;
    uint32_t pending_seek;
    volatile bool cmd_result;
    bool httpd_up;

    /* chosen streaming target */
    StreamDevice cur_device;
    bool have_device;

    /* WiFi scan / connect scene state */
    StreamingApRecord* ap_records; /* STREAMING_MAX_APS slots */
    uint16_t ap_count;
    size_t ap_selected_index;
    StreamingApRecord target_ap;
    char password_input[STREAMING_PASSWORD_MAX];

    /* device scan (mixed AirPlay + Cast/DLNA), async worker */
    StreamDevice devices[STREAMING_MAX_DEVICES];
    uint8_t device_count;
    FuriThread* scan_thread;
    volatile bool scan_busy;
};
