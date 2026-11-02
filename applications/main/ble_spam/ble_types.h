#pragma once

#include <stdint.h>

#include <sdkconfig.h>

#if CONFIG_BT_NIMBLE_ENABLED
typedef uint8_t BleAddress[6];
typedef uint8_t BleAddressType;

/* Keep the same public shape as the old ESP-IDF UUID used by the UI/log code. */
typedef struct {
    uint16_t len;
    union {
        uint16_t uuid16;
        uint32_t uuid32;
        uint8_t uuid128[16];
    } uuid;
} BleUuid;

#define ESP_UUID_LEN_16 2
#define ESP_UUID_LEN_32 4
#define ESP_UUID_LEN_128 16
#else
#include <esp_gap_ble_api.h>
#include <esp_bt_defs.h>

typedef esp_bd_addr_t BleAddress;
typedef esp_ble_addr_type_t BleAddressType;
typedef esp_bt_uuid_t BleUuid;
#endif
