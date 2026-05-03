#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_gap_ble_api.h>

#define TRACKER_MAX_DEVICES 24

typedef enum {
    TrackerKindUnknown,
    TrackerKindAirTag,        // Apple FindMy: mfg 0x004C + sub 0x12
    TrackerKindSmartTag,      // Samsung: mfg 0x0075
    TrackerKindTile,          // Service UUID 0xFEED / 0xFD84
    TrackerKindApplePP,       // Apple Proximity Pairing: mfg 0x004C + sub 0x07 (AirPods etc.)
    TrackerKindAppleNearby,   // Apple Nearby: mfg 0x004C + sub 0x10 (iPhone/Watch)
} TrackerKind;

typedef struct {
    uint8_t addr[6];
    esp_ble_addr_type_t addr_type;
    TrackerKind kind;
    char name[24];        // model name (Apple PP) or empty
    int8_t rssi;
    int8_t best_rssi;
    uint32_t last_seen_ms;
} TrackerDevice;

bool ble_tracker_hal_start_scan(void);
void ble_tracker_hal_stop_scan(void);
TrackerDevice* ble_tracker_hal_get_devices(uint16_t* count);

const char* ble_tracker_kind_label(TrackerKind k);
