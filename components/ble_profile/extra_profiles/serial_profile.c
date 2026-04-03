/**
 * @file serial_profile.c
 * BLE Serial Profile — ESP32 implementation
 */

#include "serial_profile.h"

#include <ble_serial.h>
#include <furi.h>
#include <furi_hal.h>

#include <stdio.h>
#include <string.h>

typedef struct {
    FuriHalBleProfileBase base;
    BleSerial* serial;
} BleProfileSerial;

static void serial_profile_reverse_mac(uint8_t dst[6], const uint8_t src[6]) {
    for(size_t i = 0; i < 6; i++) {
        dst[i] = src[6 - 1 - i];
    }
}

static void serial_profile_fill_default_gap_config(GapConfig* config) {
    furi_check(config);
    memset(config, 0, sizeof(*config));

    /* MAC left as all-zeros → use public BLE address (no random addr) */

    config->bonding_mode = true;
    config->pairing_method = GapPairingPinCodeVerifyYesNo;
    snprintf(
        config->adv_name,
        sizeof(config->adv_name),
        "Flipper %s",
        furi_hal_version_get_name_ptr());
}

FuriHalBleProfileBase* ble_profile_serial_start_with_config(
    const FuriHalBleProfileTemplate* profile_template,
    const GapConfig* config) {

    if(!profile_template || !config) return NULL;

    BleProfileSerial* profile = calloc(1, sizeof(BleProfileSerial));
    if(!profile) return NULL;

    profile->base.config = profile_template;

    BleSerialConfig serial_config = {0};
    strlcpy(serial_config.device_name, config->adv_name, sizeof(serial_config.device_name));
    serial_profile_reverse_mac(serial_config.mac, config->mac_address);
    serial_config.bonding = config->bonding_mode;
    serial_config.pairing = (BleSerialPairingMode)config->pairing_method;

    profile->serial = ble_serial_alloc(&serial_config);
    if(!profile->serial) {
        free(profile);
        return NULL;
    }

    return &profile->base;
}

static FuriHalBleProfileBase* serial_profile_start(FuriHalBleProfileParams profile_params) {
    (void)profile_params;
    GapConfig config = {0};
    serial_profile_fill_default_gap_config(&config);
    return ble_profile_serial_start_with_config(ble_profile_serial, &config);
}

static void serial_profile_stop(FuriHalBleProfileBase* profile) {
    BleProfileSerial* sp = (BleProfileSerial*)profile;
    if(!sp) return;
    if(sp->serial) ble_serial_free(sp->serial);
    free(sp);
}

static void serial_profile_get_config(GapConfig* config, FuriHalBleProfileParams profile_params) {
    (void)profile_params;
    serial_profile_fill_default_gap_config(config);
}

static const FuriHalBleProfileTemplate serial_profile_callbacks = {
    .start = serial_profile_start,
    .stop = serial_profile_stop,
    .get_gap_config = serial_profile_get_config,
};

const FuriHalBleProfileTemplate* ble_profile_serial = &serial_profile_callbacks;

static BleProfileSerial* serial_profile_cast(FuriHalBleProfileBase* profile) {
    return (BleProfileSerial*)profile;
}

void ble_profile_serial_set_state_callback(
    FuriHalBleProfileBase* profile,
    void (*callback)(bool connected, void* context),
    void* context) {
    BleProfileSerial* sp = serial_profile_cast(profile);
    if(!sp || !sp->serial) {
        if(callback) callback(false, context);
        return;
    }
    ble_serial_set_state_callback(sp->serial, callback, context);
}

bool ble_profile_serial_is_connected(FuriHalBleProfileBase* profile) {
    BleProfileSerial* sp = serial_profile_cast(profile);
    return sp && sp->serial && ble_serial_is_connected(sp->serial);
}

bool ble_profile_serial_tx(FuriHalBleProfileBase* profile, uint8_t* data, uint16_t size) {
    BleProfileSerial* sp = serial_profile_cast(profile);
    return sp && sp->serial && ble_serial_tx(sp->serial, data, size);
}

void ble_profile_serial_set_event_callback(
    FuriHalBleProfileBase* profile,
    uint16_t buff_size,
    SerialServiceEventCallback callback,
    void* context) {
    BleProfileSerial* sp = serial_profile_cast(profile);
    if(!sp || !sp->serial) return;
    ble_serial_set_event_callback(sp->serial, buff_size, callback, context);
}

void ble_profile_serial_set_rpc_active(
    FuriHalBleProfileBase* profile,
    FuriHalBtSerialRpcStatus status) {
    BleProfileSerial* sp = serial_profile_cast(profile);
    if(!sp || !sp->serial) return;
    ble_serial_set_rpc_active(sp->serial, status);
}

void ble_profile_serial_notify_buffer_is_empty(FuriHalBleProfileBase* profile) {
    BleProfileSerial* sp = serial_profile_cast(profile);
    if(!sp || !sp->serial) return;
    ble_serial_notify_buffer_is_empty(sp->serial);
}
