#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <furi_ble/profile_interface.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const FuriHalBleProfileTemplate* ble_profile_hid;

FuriHalBleProfileBase* ble_profile_hid_start_with_config(
    const FuriHalBleProfileTemplate* profile_template,
    const GapConfig* config);

void ble_profile_hid_set_state_callback(
    FuriHalBleProfileBase* profile,
    void (*callback)(bool connected, void* context),
    void* context);

bool ble_profile_hid_is_connected(FuriHalBleProfileBase* profile);
uint8_t ble_profile_hid_get_led_state(FuriHalBleProfileBase* profile);

bool ble_profile_hid_kb_press(FuriHalBleProfileBase* profile, uint16_t button);
bool ble_profile_hid_kb_release(FuriHalBleProfileBase* profile, uint16_t button);
bool ble_profile_hid_kb_release_all(FuriHalBleProfileBase* profile);

bool ble_profile_hid_consumer_key_press(FuriHalBleProfileBase* profile, uint16_t button);
bool ble_profile_hid_consumer_key_release(FuriHalBleProfileBase* profile, uint16_t button);
bool ble_profile_hid_consumer_key_release_all(FuriHalBleProfileBase* profile);

bool ble_profile_hid_mouse_move(FuriHalBleProfileBase* profile, int8_t dx, int8_t dy);
bool ble_profile_hid_mouse_press(FuriHalBleProfileBase* profile, uint8_t button);
bool ble_profile_hid_mouse_release(FuriHalBleProfileBase* profile, uint8_t button);
bool ble_profile_hid_mouse_release_all(FuriHalBleProfileBase* profile);
bool ble_profile_hid_mouse_scroll(FuriHalBleProfileBase* profile, int8_t delta);

#ifdef __cplusplus
}
#endif
