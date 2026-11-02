#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_CHANNEL_MIN          1
#define SD_CHANNEL_MAX          13
#define SD_MAX_APS              16
#define SD_MAX_CLIENTS          32
#define SD_MAX_CH_CLIENTS       32

#define SD_SCAN_DWELL_MS        230
#define SD_SCAN_SETTLE_MS       15
#define SD_DISCOVERY_MS         500

#define SD_RSSI_MEDIAN_WINDOW   5
#define SD_RSSI_REF_1M          (-40)
#define SD_PATH_LOSS_N          3.0f

typedef enum {
    SdViewModeScan = 0,
    SdViewModeAttack,
    SdViewModeMonitor,
} SdViewMode;

typedef enum {
    SD_PMF_NONE = 0,
    SD_PMF_CAPABLE,
    SD_PMF_REQUIRED,
} SdPmfState;

typedef struct {
    int8_t   history[SD_RSSI_MEDIAN_WINDOW];
    uint8_t  idx;
    uint8_t  count;
    int8_t   filtered;
    int8_t   raw;
} SdRssiFilter;

typedef struct {
    uint8_t  mac[6];
    int8_t   rssi;
    uint32_t packets;
    bool     valid;
} SdClientEntry;

typedef struct {
    uint8_t       channel;
    uint8_t       ap_count;
    uint8_t       client_count;
    int8_t        rssi;
    int8_t        rssi_raw;
    char          ssid[33];
    uint8_t       bssid[6];
    SdPmfState    pmf;
    uint32_t      beacon_count;
    bool          has_ap;
    SdClientEntry clients[SD_MAX_CH_CLIENTS];
    uint8_t       client_slot_count;
} SdChannelInfo;

/* Versi ramping untuk view model — hanya field yang digambar di mode Scan
 * (tanpa clients[]/bssid/pmf/...). Menekan SdViewModel dari ~8KB → ~1.1KB. */
typedef struct {
    uint8_t channel;
    uint8_t ap_count;
    uint8_t client_count;
    int8_t  rssi;
    char    ssid[33];
    bool    has_ap;
} SdChannelView;

typedef struct {
    SdViewMode mode;
    bool       running;

    SdChannelView channels[13];
    uint8_t       detected_map[13];   // display pos -> index into channels[] (hanya AP terdeteksi)
    uint8_t       detected_count;     // jumlah kanal dengan AP terdeteksi
    uint8_t       scan_cursor;
    bool          scanning;

    uint8_t  attack_channel;
    char     attack_ssid[33];
    uint32_t attack_frames;
    uint32_t attack_fps;
    uint8_t  attack_clients;
    uint32_t attack_beacon_age_ms;
    bool     attack_beacon_alive;
    bool     attack_running;

    uint8_t       monitor_channel;
    char          monitor_ssid[33];
    uint8_t       monitor_bssid[6];
    int8_t        monitor_rssi;
    SdClientEntry monitor_clients[SD_MAX_CLIENTS];
    uint8_t       monitor_client_count;
    uint32_t      monitor_beacon_count;
    uint32_t      monitor_data_count;
    uint32_t      monitor_rate_bps;   // throughput data frame target (byte/detik)
    bool          monitor_running;
} SdViewModel;

void        wlan_smart_deauth_start(void);
void        wlan_smart_deauth_stop(void);
bool        wlan_smart_deauth_is_running(void);
SdViewMode  wlan_smart_deauth_get_mode(void);
void        wlan_smart_deauth_scan_now(void);
bool        wlan_smart_deauth_select_channel(uint8_t idx);
void        wlan_smart_deauth_attack_toggle(void);
void        wlan_smart_deauth_monitor_enter(void);
void        wlan_smart_deauth_monitor_exit(void);
void        wlan_smart_deauth_back_to_scan(void);
void        wlan_smart_deauth_get_view_model(SdViewModel* out);

#ifdef __cplusplus
}
#endif
