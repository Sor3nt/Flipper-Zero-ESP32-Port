#pragma once
#include "wardriver_nmea.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { WardriverWifi, WardriverBle } WardriverMode;
enum {
    WardriverHintFlock = 1U << 0,
    WardriverHintAxon = 1U << 1,
    WardriverHintTracker = 1U << 2,
    WardriverHintSerial = 1U << 3,
    WardriverHintRemoteId = 1U << 4,
    WardriverHintXuntong = 1U << 5,
};
/* Supplier OUIs and generic UART modules alone are not notable-device hints. */
#define WARDRIVER_NOTABLE_HINTS (WardriverHintFlock | WardriverHintAxon | WardriverHintTracker | WardriverHintRemoteId)
typedef struct {
    uint8_t mac[6];
    char ssid[33];
    int8_t rssi;
    uint8_t channel, auth, pairwise, group;
    bool random_address;
    WardriverMode mode;
    uint32_t hints;
    uint32_t seen_ms;
    char remote_id[21];
} WardriverObservation;

typedef struct {
    bool used, dirty;
    WardriverObservation observation;
    char vendor[29];
    uint32_t detections;
    uint64_t first_seen, last_seen;
    uint32_t first_seen_ms, last_seen_ms;
    WardriverGpsData gps;      /* This observation only; used by the logger. */
    WardriverGpsData last_fix; /* Retain the last actual fix for device details. */
} WardriverNetwork;

typedef struct {
    WardriverNetwork* entries;
    size_t capacity, count, wifi_count, ble_count, notable_count;
    uint64_t detections;
    uint32_t overflow;
} WardriverTable;

WardriverNetwork* wardriver_table_observe(WardriverTable* table, const WardriverObservation* observation,
                                        uint64_t timestamp, const WardriverGpsData* gps, bool* is_new);
void wardriver_mac_text(const uint8_t mac[6], char out[18]);
const char* wardriver_hint_text(uint32_t flags);
