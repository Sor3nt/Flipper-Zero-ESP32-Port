#include "ble_hid_ext_profile.h"

#include <furi.h>

static void
    ble_profile_hid_ext_get_config(GapConfig* config, FuriHalBleProfileParams profile_params) {
    furi_check(config);
    furi_check(profile_params);
    BleProfileHidExtParams* hid_ext_profile_params = profile_params;

    // Setup config with basic profile
    ble_profile_hid->get_gap_config(config, NULL);

    if(hid_ext_profile_params->name[0] != '\0') {
        // adv_name is the plain device name on this port (no leading ADV type byte as on STM32)
        strlcpy(config->adv_name, hid_ext_profile_params->name, sizeof(config->adv_name));
    }
}

static FuriHalBleProfileBase* ble_profile_hid_ext_start(FuriHalBleProfileParams profile_params) {
    if(!profile_params) {
        return ble_profile_hid->start(NULL);
    }

    // The HAL on this port never calls get_gap_config(): the config has to be applied here,
    // otherwise the name chosen in the app is silently dropped.
    GapConfig config = {0};
    ble_profile_hid_ext_get_config(&config, profile_params);
    // Start with the base HID template: the HAL dispatches state callbacks and advertising on it.
    return ble_profile_hid_start_with_config(ble_profile_hid, &config);
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
