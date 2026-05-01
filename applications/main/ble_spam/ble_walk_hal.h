#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>

#define BLE_WALK_MAX_DEVICES    32
#define BLE_WALK_MAX_SERVICES   16
#define BLE_WALK_MAX_CHARS      32
#define BLE_WALK_MAX_VALUE_LEN  128

typedef struct {
    esp_bd_addr_t addr;
    esp_ble_addr_type_t addr_type;
    int8_t rssi;
    char name[32];
    uint8_t adv_data[31];
    uint8_t adv_data_len;
    uint8_t scan_rsp_data[31];
    uint8_t scan_rsp_len;
} BleWalkDevice;

typedef struct {
    esp_bt_uuid_t uuid;
    uint16_t start_handle;
    uint16_t end_handle;
} BleWalkService;

typedef struct {
    esp_bt_uuid_t uuid;
    uint16_t handle;
    uint8_t properties;
} BleWalkChar;

// Lifecycle
bool ble_walk_hal_start(void);
void ble_walk_hal_stop(void);

// Scanning
bool ble_walk_hal_start_scan(void);
void ble_walk_hal_stop_scan(void);
bool ble_walk_hal_is_scanning(void);
BleWalkDevice* ble_walk_hal_get_devices(uint16_t* count);

// GATT Client
bool ble_walk_hal_connect(BleWalkDevice* device);
void ble_walk_hal_disconnect(void);
bool ble_walk_hal_is_connected(void);

bool ble_walk_hal_discover_services(void);
bool ble_walk_hal_services_ready(void);
BleWalkService* ble_walk_hal_get_services(uint16_t* count);

bool ble_walk_hal_discover_chars(BleWalkService* service);
bool ble_walk_hal_chars_ready(void);
BleWalkChar* ble_walk_hal_get_chars(uint16_t* count);

bool ble_walk_hal_read_char(uint16_t handle);
bool ble_walk_hal_read_ready(void);
uint8_t* ble_walk_hal_get_read_value(uint16_t* len);
uint8_t ble_walk_hal_get_read_status(void);

bool ble_walk_hal_write_char(uint16_t handle, const uint8_t* data, uint16_t len);
