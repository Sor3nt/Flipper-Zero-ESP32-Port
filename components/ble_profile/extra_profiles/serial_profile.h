/**
 * @file serial_profile.h
 * BLE Serial Profile — ESP32 implementation
 *
 * Wraps ble_serial as a FuriHalBleProfileTemplate so bt_service can
 * use it as the default profile (matching STM32 ble_profile_serial).
 */

#pragma once

#include <furi_ble/profile_interface.h>
#include <ble_serial.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const FuriHalBleProfileTemplate* ble_profile_serial;

FuriHalBleProfileBase* ble_profile_serial_start_with_config(
    const FuriHalBleProfileTemplate* profile_template,
    const GapConfig* config);

void ble_profile_serial_set_state_callback(
    FuriHalBleProfileBase* profile,
    void (*callback)(bool connected, void* context),
    void* context);

bool ble_profile_serial_is_connected(FuriHalBleProfileBase* profile);

bool ble_profile_serial_tx(FuriHalBleProfileBase* profile, uint8_t* data, uint16_t size);

void ble_profile_serial_set_event_callback(
    FuriHalBleProfileBase* profile,
    uint16_t buff_size,
    SerialServiceEventCallback callback,
    void* context);

void ble_profile_serial_set_rpc_active(
    FuriHalBleProfileBase* profile,
    FuriHalBtSerialRpcStatus status);

void ble_profile_serial_notify_buffer_is_empty(FuriHalBleProfileBase* profile);

#ifdef __cplusplus
}
#endif
