#pragma once

#include <ble_profile/extra_profiles/hid_profile.h>
#include <furi_ble/profile_interface.h>

typedef struct {
    char name[FURI_HAL_BT_ADV_NAME_LENGTH];
    uint8_t mac[GAP_MAC_ADDR_SIZE];
    bool bonding;
    GapPairing pairing;
} BleProfileHidExtParams;

extern const FuriHalBleProfileTemplate* ble_profile_hid_ext;
