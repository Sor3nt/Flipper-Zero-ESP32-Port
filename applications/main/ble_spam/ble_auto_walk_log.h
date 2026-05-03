#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_gap_ble_api.h>

#include "ble_walk_hal.h"

#define BLE_AUTO_WALK_DIR  "/ext/ble"
#define BLE_AUTO_WALK_PATH "/ext/ble/auto_walk.csv"
#define BLE_AUTO_WALK_SEEN_MAX 256

typedef struct {
    esp_bd_addr_t addrs[BLE_AUTO_WALK_SEEN_MAX];
    uint16_t count;
} BleAutoWalkSeenSet;

typedef struct BleAutoWalkLog BleAutoWalkLog;

void ble_auto_walk_seen_reset(BleAutoWalkSeenSet* set);
bool ble_auto_walk_seen_contains(const BleAutoWalkSeenSet* set, const esp_bd_addr_t addr);
bool ble_auto_walk_seen_add(BleAutoWalkSeenSet* set, const esp_bd_addr_t addr);

// Opens (or creates) the CSV. Reads existing entries and populates `out_seen`
// with all unique addresses already crawled. Returns NULL on failure.
BleAutoWalkLog* ble_auto_walk_log_open(BleAutoWalkSeenSet* out_seen);

void ble_auto_walk_log_close(BleAutoWalkLog* log);

// One row when a device couldn't be crawled (connect_failed, no_services).
void ble_auto_walk_log_device_marker(
    BleAutoWalkLog* log,
    const BleWalkDevice* device,
    const char* status);

// One row per characteristic value read.
void ble_auto_walk_log_char_value(
    BleAutoWalkLog* log,
    const BleWalkDevice* device,
    const BleWalkService* service,
    const BleWalkChar* chr,
    const uint8_t* value,
    uint16_t value_len);
