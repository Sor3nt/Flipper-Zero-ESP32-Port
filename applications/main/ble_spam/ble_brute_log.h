#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ble_walk_hal.h"

#define BLE_BRUTE_DIR  "/ext/ble"
#define BLE_BRUTE_PATH "/ext/ble/handle_brute.csv"

typedef struct BleBruteLog BleBruteLog;

BleBruteLog* ble_brute_log_open(void);
void ble_brute_log_close(BleBruteLog* log);

// Marker line at session start / on connect failure
void ble_brute_log_session_marker(
    BleBruteLog* log,
    const BleWalkDevice* device,
    const char* event);

// One row per non-INVALID-handle response
void ble_brute_log_hit(
    BleBruteLog* log,
    const BleWalkDevice* device,
    uint16_t handle,
    uint8_t status,
    const uint8_t* value,
    uint16_t value_len);

const char* ble_brute_status_name(uint8_t status);
