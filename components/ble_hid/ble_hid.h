#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_HID_DEVICE_NAME_LEN 32
#define BLE_HID_PASSKEY_DEFAULT 123456U

typedef enum {
    BleHidPairingModeVerifyYesNo = 0,
    BleHidPairingModeDisplayOnly,
    BleHidPairingModeInputOnly,
} BleHidPairingMode;

typedef struct BleHid BleHid;

typedef struct {
    char device_name[BLE_HID_DEVICE_NAME_LEN + 1];
    uint8_t mac[6];
    bool bonding;
    BleHidPairingMode pairing;
} BleHidConfig;

typedef void (*BleHidStateCallback)(bool connected, void* context);

void ble_hid_get_default_mac(uint8_t mac[6]);

BleHid* ble_hid_alloc(const BleHidConfig* config);

void ble_hid_free(BleHid* ble_hid);

void ble_hid_set_state_callback(BleHid* ble_hid, BleHidStateCallback callback, void* context);

bool ble_hid_is_connected(BleHid* ble_hid);

bool ble_hid_kb_press(BleHid* ble_hid, uint16_t button);
bool ble_hid_kb_release(BleHid* ble_hid, uint16_t button);
bool ble_hid_kb_release_all(BleHid* ble_hid);

bool ble_hid_mouse_move(BleHid* ble_hid, int8_t dx, int8_t dy);
bool ble_hid_mouse_press(BleHid* ble_hid, uint8_t button);
bool ble_hid_mouse_release(BleHid* ble_hid, uint8_t button);
bool ble_hid_mouse_release_all(BleHid* ble_hid);
bool ble_hid_mouse_scroll(BleHid* ble_hid, int8_t delta);

bool ble_hid_consumer_press(BleHid* ble_hid, uint16_t button);
bool ble_hid_consumer_release(BleHid* ble_hid, uint16_t button);
bool ble_hid_consumer_release_all(BleHid* ble_hid);

uint8_t ble_hid_get_led_state(BleHid* ble_hid);

bool ble_hid_start_advertising(void);
void ble_hid_stop_advertising(void);
bool ble_hid_is_advertising(void);
bool ble_hid_is_active(void);

bool ble_hid_remove_pairing(void);

/* Clear the "stack already initialized" latch so the next ble_hid_alloc() re-runs
 * controller/bluedroid bring-up. Must be called whenever the BLE stack is torn
 * down from outside this component (bt_handle_stop_stack), mirroring
 * ble_serial_reset_initialized(). Safe to call when BT is off. */
void ble_hid_reset_initialized(void);

#ifdef __cplusplus
}
#endif
