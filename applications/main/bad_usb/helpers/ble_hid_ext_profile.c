#include "ble_hid_ext_profile.h"

#include <furi.h>
#include <string.h>

static void
    ble_profile_hid_ext_get_config(GapConfig* config, FuriHalBleProfileParams profile_params) {
    BleProfileHidExtParams* hid_ext_profile_params = profile_params;

    furi_check(config);
    furi_check(profile_params);

    ble_profile_hid->get_gap_config(config, NULL);
    memcpy(config->mac_address, hid_ext_profile_params->mac, sizeof(config->mac_address));
    strlcpy(config->adv_name, hid_ext_profile_params->name, sizeof(config->adv_name));
    config->bonding_mode = hid_ext_profile_params->bonding;
    config->pairing_method = hid_ext_profile_params->pairing;
}

static FuriHalBleProfileBase* ble_profile_hid_ext_start(FuriHalBleProfileParams profile_params) {
    if(!profile_params) {
        return ble_profile_hid->start(NULL);
    }

    /* The HAL on this port never calls get_gap_config(): apply the BadUSB name,
     * MAC, and bonding preference here. BadUSB pairing is intentionally Just Works. */
    GapConfig config = {0};
    ble_profile_hid_ext_get_config(&config, profile_params);
    /* BadUSB BLE HID uses Just Works: Windows should not ask for a PIN or
     * numeric-comparison confirmation. This flag is not set by other profiles. */
    return ble_profile_hid_start_with_config_just_works(ble_profile_hid, &config);
}

static bool ble_profile_hid_ext_stop(FuriHalBleProfileBase* profile) {
    return ble_profile_hid->stop(profile);
}

static const FuriHalBleProfileTemplate profile_callbacks = {
    .start = ble_profile_hid_ext_start,
    .stop = ble_profile_hid_ext_stop,
    .get_gap_config = ble_profile_hid_ext_get_config,
};

const FuriHalBleProfileTemplate* ble_profile_hid_ext = &profile_callbacks;
