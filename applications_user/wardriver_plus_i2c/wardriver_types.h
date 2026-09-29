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
    WardriverHintAirTag = 1U << 6,
    WardriverHintFindMy = 1U << 7,
    WardriverHintSmartTag = 1U << 8,
    WardriverHintTile = 1U << 9,
    WardriverHintChipolo = 1U << 10,
    WardriverHintAirPods = 1U << 11,
    WardriverHintMeta = 1U << 12,
    WardriverHintRaven = 1U << 13,
    WardriverHintCamera = 1U << 14,
    WardriverHintRing = 1U << 15,
    WardriverHintAlpr = 1U << 16,
    WardriverHintGoogle = 1U << 17,
    WardriverHintIBeacon = 1U << 18,
    WardriverHintFlipper = 1U << 19,
    WardriverHintPwnagotchi = 1U << 20,
    WardriverHintPineapple = 1U << 21,
    WardriverHintDeauther = 1U << 22,
    WardriverHintDeauth = 1U << 23,
    WardriverHintEvilTwin = 1U << 24,
    WardriverHintEddystone = 1U << 25,
    WardriverHintGlasses = 1U << 26,
    WardriverHintSkimmer = 1U << 27,
};
/* Supplier OUIs and generic UART modules alone are not notable-device hints. */
#define WARDRIVER_NOTABLE_HINTS (WardriverHintFlock | WardriverHintAxon | WardriverHintTracker | WardriverHintRemoteId | WardriverHintMeta | WardriverHintRaven | WardriverHintCamera | WardriverHintRing | WardriverHintAlpr | WardriverHintFlipper | WardriverHintPwnagotchi | WardriverHintPineapple | WardriverHintDeauther | WardriverHintDeauth | WardriverHintEvilTwin | WardriverHintGlasses | WardriverHintSkimmer)
/* Beacons remain visible in All devices, without overwhelming Notable. */
typedef enum { WardConfidenceNone, WardConfidenceLow, WardConfidenceMedium, WardConfidenceHigh } WardriverConfidence;
typedef enum { WardEvidenceNone, WardEvidenceName, WardEvidenceService, WardEvidenceCompany, WardEvidenceOui, WardEvidencePayload, WardEvidenceBehavior } WardriverEvidence;
enum { WardRidLocation=1, WardRidPosition=2, WardRidAltitude=4, WardRidSystem=8, WardRidOperator=16, WardRidBasic=32 };
typedef struct {
    uint8_t fields, aircraft, operator_type;
    int32_t latitude, longitude, operator_latitude, operator_longitude;
    uint16_t altitude; /* Half metres, offset by -1000; zero is unknown. */
} WardriverRemoteId;
typedef struct {
    uint8_t mac[6];
    char ssid[33];
    int8_t rssi;
    uint8_t channel, auth, pairwise, group;
    bool random_address;
    WardriverMode mode;
    uint32_t hints, primary_hint;
    WardriverConfidence confidence;
    WardriverEvidence evidence;
    WardriverRemoteId rid;
    uint32_t seen_ms;
    char remote_id[21];
} WardriverObservation;

typedef struct {
    bool used, dirty;
    WardriverObservation observation;
    char vendor[29];
    uint32_t detections;
    uint64_t first_seen, last_seen;
    uint32_t first_seen_ms, last_seen_ms, rid_location_ms, rid_operator_ms;
    int8_t previous_rssi;
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

const char* wardriver_device_label(uint32_t hints);
const char* wardriver_confidence_text(WardriverConfidence confidence);
const char* wardriver_evidence_text(WardriverEvidence evidence);

const char* wardriver_filter_name(unsigned filter);
bool wardriver_filter_match(const WardriverObservation* observation,unsigned filter,WardriverConfidence minimum);
