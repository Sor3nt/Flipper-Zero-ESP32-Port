#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIVE_SCAN_MAX_APS       64
#define LIVE_SCAN_DWELL_MS      300
#define LIVE_SCAN_HOP_MS        300

typedef enum {
    LiveSortRssi = 0,
    LiveSortChannel,
    LiveSortSsid,
    LiveSortFirstSeen,
} LiveSortMode;

typedef enum {
    LiveFilterAll = 0,
    LiveFilterOpen,
    LiveFilterWpa2,
    LiveFilterWpa3,
} LiveFilterMode;

typedef struct {
    // Identitas
    uint8_t  bssid[6];
    char     ssid[33];
    char     vendor[24];

    // Radio
    uint8_t  channel;
    int8_t   rssi;
    int8_t   rssi_max;
    int8_t   rssi_min;

    // Keamanan
    uint8_t  authmode;      // 0=open 1=WEP/privacy 3=WPA2(RSN) 4=WPA3(SAE)
    bool     has_rsn;
    bool     has_wpa3;
    bool     has_pmf;
    uint16_t capabilities;

    // Timing
    uint32_t first_seen_ms;
    uint32_t last_seen_ms;
    uint32_t beacon_count;

    bool     valid;
} LiveApEntry;

bool        wlan_live_scan_start(void);
void        wlan_live_scan_stop(void);
bool        wlan_live_scan_is_running(void);

void        wlan_live_scan_clear(void);
void        wlan_live_scan_pause(void);
void        wlan_live_scan_resume(void);

uint16_t    wlan_live_scan_get_count(void);
uint16_t    wlan_live_scan_snapshot(LiveApEntry* out, uint16_t max);

void        wlan_live_scan_set_sort(LiveSortMode mode);
void        wlan_live_scan_set_filter(LiveFilterMode filter);
LiveSortMode   wlan_live_scan_get_sort(void);
LiveFilterMode wlan_live_scan_get_filter(void);

void        wlan_live_scan_set_hop(bool enabled);
bool        wlan_live_scan_is_hopping(void);
void        wlan_live_scan_set_channel(uint8_t channel);
uint8_t     wlan_live_scan_get_channel(void);

#ifdef __cplusplus
}
#endif
