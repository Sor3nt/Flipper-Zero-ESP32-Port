/**
 * @file ble_serial.h
 * BLE Serial Service for ESP32
 *
 * Implements the Flipper Zero serial GATT service with TX/RX/Flow/RPC
 * characteristics, matching the STM32 serial_service UUIDs so the
 * Flipper mobile app can connect.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_SERIAL_DEVICE_NAME_LEN 32

/** Max data size for a single serial transfer (matches STM32) */
#define BLE_SVC_SERIAL_DATA_LEN_MAX       (486)
/** Max value per ATT packet */
#define BLE_SVC_SERIAL_CHAR_VALUE_LEN_MAX (243)
/** Default packet size for serial profile */
#define BLE_PROFILE_SERIAL_PACKET_SIZE_MAX BLE_SVC_SERIAL_CHAR_VALUE_LEN_MAX

typedef struct BleSerial BleSerial;

typedef enum {
    BleSerialPairingPinCodeVerifyYesNo = 0,
    BleSerialPairingDisplayOnly,
    BleSerialPairingInputOnly,
} BleSerialPairingMode;

typedef struct {
    char device_name[BLE_SERIAL_DEVICE_NAME_LEN + 1];
    uint8_t mac[6];
    bool bonding;
    BleSerialPairingMode pairing;
} BleSerialConfig;

typedef enum {
    SerialServiceEventTypeDataReceived,
    SerialServiceEventTypeDataSent,
    SerialServiceEventTypesBleResetRequest,
} SerialServiceEventType;

typedef struct {
    uint8_t* buffer;
    uint16_t size;
} SerialServiceData;

typedef struct {
    SerialServiceEventType event;
    SerialServiceData data;
} SerialServiceEvent;

typedef uint16_t (*SerialServiceEventCallback)(SerialServiceEvent event, void* context);

typedef void (*BleSerialStateCallback)(bool connected, void* context);

typedef enum {
    FuriHalBtSerialRpcStatusNotActive = 0,
    FuriHalBtSerialRpcStatusActive = 1,
} FuriHalBtSerialRpcStatus;

BleSerial* ble_serial_alloc(const BleSerialConfig* config);
void ble_serial_free(BleSerial* serial);

/** Reset initialized flag so stack can be re-initialized after deinit */
void ble_serial_reset_initialized(void);

void ble_serial_set_state_callback(BleSerial* serial, BleSerialStateCallback callback, void* context);
bool ble_serial_is_connected(BleSerial* serial);

void ble_serial_set_event_callback(
    BleSerial* serial,
    uint16_t buff_size,
    SerialServiceEventCallback callback,
    void* context);

bool ble_serial_tx(BleSerial* serial, uint8_t* data, uint16_t size);
void ble_serial_set_rpc_active(BleSerial* serial, FuriHalBtSerialRpcStatus status);
void ble_serial_notify_buffer_is_empty(BleSerial* serial);

bool ble_serial_start_advertising(void);
void ble_serial_stop_advertising(void);
bool ble_serial_is_advertising(void);
bool ble_serial_is_active(void);

bool ble_serial_remove_pairing(void);

void ble_serial_get_default_mac(uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
