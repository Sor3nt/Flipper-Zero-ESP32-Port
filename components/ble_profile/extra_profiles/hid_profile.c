#include "hid_profile.h"

#include <ble_hid.h>
#include <furi.h>
#include <furi_hal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FuriHalBleProfileBase base;
    BleHid* ble_hid;
} BleProfileHid;

typedef struct {
    uint16_t mac_xor;
    const char* device_name_prefix;
} BleProfileHidParams;

static void ble_profile_hid_reverse_mac(
    uint8_t dst[GAP_MAC_ADDR_SIZE],
    const uint8_t src[GAP_MAC_ADDR_SIZE]) {
    for(size_t i = 0; i < GAP_MAC_ADDR_SIZE; i++) {
        dst[i] = src[GAP_MAC_ADDR_SIZE - 1 - i];
    }
}

static void ble_profile_hid_fill_default_gap_config(
    GapConfig* config,
    const BleProfileHidParams* params) {
    uint8_t default_mac[GAP_MAC_ADDR_SIZE];
    const char* device_name_prefix = "Control";

    furi_check(config);
    memset(config, 0, sizeof(*config));

    ble_hid_get_default_mac(default_mac);
    ble_profile_hid_reverse_mac(config->mac_address, default_mac);
    config->mac_address[2]++;

    if(params) {
        config->mac_address[0] ^= params->mac_xor;
        config->mac_address[1] ^= params->mac_xor >> 8;
        if(params->device_name_prefix) {
            device_name_prefix = params->device_name_prefix;
        }
    }

    config->bonding_mode = true;
    config->pairing_method = GapPairingPinCodeVerifyYesNo;
    snprintf(
        config->adv_name,
        sizeof(config->adv_name),
        "%s %s",
        device_name_prefix,
        furi_hal_version_get_name_ptr());
}

FuriHalBleProfileBase* ble_profile_hid_start_with_config(
    const FuriHalBleProfileTemplate* profile_template,
    const GapConfig* config) {
    BleProfileHid* profile = NULL;
    BleHidConfig ble_config = {0};

    if(!profile_template || !config) {
        return NULL;
    }

    profile = calloc(1, sizeof(BleProfileHid));
    if(!profile) {
        return NULL;
    }

    profile->base.config = profile_template;
    strlcpy(ble_config.device_name, config->adv_name, sizeof(ble_config.device_name));
    ble_profile_hid_reverse_mac(ble_config.mac, config->mac_address);
    ble_config.bonding = config->bonding_mode;
    ble_config.pairing = (BleHidPairingMode)config->pairing_method;

    profile->ble_hid = ble_hid_alloc(&ble_config);
    if(!profile->ble_hid) {
        free(profile);
        return NULL;
    }

    return &profile->base;
}

static FuriHalBleProfileBase* ble_profile_hid_start(FuriHalBleProfileParams profile_params) {
    GapConfig config = {0};

    ble_profile_hid_fill_default_gap_config(&config, profile_params);
    return ble_profile_hid_start_with_config(ble_profile_hid, &config);
}

static void ble_profile_hid_stop(FuriHalBleProfileBase* profile) {
    BleProfileHid* hid_profile = (BleProfileHid*)profile;

    if(!hid_profile) {
        return;
    }

    if(hid_profile->ble_hid) {
        ble_hid_free(hid_profile->ble_hid);
    }

    free(hid_profile);
}

static void ble_profile_hid_get_config(GapConfig* config, FuriHalBleProfileParams profile_params) {
    ble_profile_hid_fill_default_gap_config(config, profile_params);
}

static const FuriHalBleProfileTemplate profile_callbacks = {
    .start = ble_profile_hid_start,
    .stop = ble_profile_hid_stop,
    .get_gap_config = ble_profile_hid_get_config,
};

const FuriHalBleProfileTemplate* ble_profile_hid = &profile_callbacks;

static BleProfileHid* ble_profile_hid_cast(FuriHalBleProfileBase* profile) {
    return (BleProfileHid*)profile;
}

void ble_profile_hid_set_state_callback(
    FuriHalBleProfileBase* profile,
    void (*callback)(bool connected, void* context),
    void* context) {
    BleProfileHid* hid_profile = ble_profile_hid_cast(profile);

    if(!hid_profile || !hid_profile->ble_hid) {
        if(callback) {
            callback(false, context);
        }
        return;
    }

    ble_hid_set_state_callback(hid_profile->ble_hid, callback, context);
}

bool ble_profile_hid_is_connected(FuriHalBleProfileBase* profile) {
    BleProfileHid* hid_profile = ble_profile_hid_cast(profile);
    return hid_profile && hid_profile->ble_hid && ble_hid_is_connected(hid_profile->ble_hid);
}

uint8_t ble_profile_hid_get_led_state(FuriHalBleProfileBase* profile) {
    BleProfileHid* hid_profile = ble_profile_hid_cast(profile);
    return hid_profile && hid_profile->ble_hid ? ble_hid_get_led_state(hid_profile->ble_hid) : 0;
}

bool ble_profile_hid_kb_press(FuriHalBleProfileBase* profile, uint16_t button) {
    BleProfileHid* hid_profile = ble_profile_hid_cast(profile);
    return hid_profile && hid_profile->ble_hid && ble_hid_kb_press(hid_profile->ble_hid, button);
}

bool ble_profile_hid_kb_release(FuriHalBleProfileBase* profile, uint16_t button) {
    BleProfileHid* hid_profile = ble_profile_hid_cast(profile);
    return hid_profile && hid_profile->ble_hid && ble_hid_kb_release(hid_profile->ble_hid, button);
}

bool ble_profile_hid_kb_release_all(FuriHalBleProfileBase* profile) {
    BleProfileHid* hid_profile = ble_profile_hid_cast(profile);
    return hid_profile && hid_profile->ble_hid && ble_hid_kb_release_all(hid_profile->ble_hid);
}

bool ble_profile_hid_consumer_key_press(FuriHalBleProfileBase* profile, uint16_t button) {
    BleProfileHid* hid_profile = ble_profile_hid_cast(profile);
    return hid_profile && hid_profile->ble_hid &&
           ble_hid_consumer_press(hid_profile->ble_hid, button);
}

bool ble_profile_hid_consumer_key_release(FuriHalBleProfileBase* profile, uint16_t button) {
    BleProfileHid* hid_profile = ble_profile_hid_cast(profile);
    return hid_profile && hid_profile->ble_hid &&
           ble_hid_consumer_release(hid_profile->ble_hid, button);
}

bool ble_profile_hid_consumer_key_release_all(FuriHalBleProfileBase* profile) {
    BleProfileHid* hid_profile = ble_profile_hid_cast(profile);
    return hid_profile && hid_profile->ble_hid &&
           ble_hid_consumer_release_all(hid_profile->ble_hid);
}

bool ble_profile_hid_mouse_move(FuriHalBleProfileBase* profile, int8_t dx, int8_t dy) {
    BleProfileHid* hid_profile = ble_profile_hid_cast(profile);
    return hid_profile && hid_profile->ble_hid &&
           ble_hid_mouse_move(hid_profile->ble_hid, dx, dy);
}

bool ble_profile_hid_mouse_press(FuriHalBleProfileBase* profile, uint8_t button) {
    BleProfileHid* hid_profile = ble_profile_hid_cast(profile);
    return hid_profile && hid_profile->ble_hid &&
           ble_hid_mouse_press(hid_profile->ble_hid, button);
}

bool ble_profile_hid_mouse_release(FuriHalBleProfileBase* profile, uint8_t button) {
    BleProfileHid* hid_profile = ble_profile_hid_cast(profile);
    return hid_profile && hid_profile->ble_hid &&
           ble_hid_mouse_release(hid_profile->ble_hid, button);
}

bool ble_profile_hid_mouse_release_all(FuriHalBleProfileBase* profile) {
    BleProfileHid* hid_profile = ble_profile_hid_cast(profile);
    return hid_profile && hid_profile->ble_hid &&
           ble_hid_mouse_release_all(hid_profile->ble_hid);
}

bool ble_profile_hid_mouse_scroll(FuriHalBleProfileBase* profile, int8_t delta) {
    BleProfileHid* hid_profile = ble_profile_hid_cast(profile);
    return hid_profile && hid_profile->ble_hid &&
           ble_hid_mouse_scroll(hid_profile->ble_hid, delta);
}
