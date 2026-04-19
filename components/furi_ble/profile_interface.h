#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FURI_HAL_BT_ADV_NAME_LENGTH
#define FURI_HAL_BT_ADV_NAME_LENGTH 32
#endif

#ifndef GAP_MAC_ADDR_SIZE
#define GAP_MAC_ADDR_SIZE 6
#endif

typedef enum {
    GapPairingPinCodeVerifyYesNo = 0,
    GapPairingPinCodeDisplayOnly,
    GapPairingPinCodeInputOnly,
    GapPairingCount,
} GapPairing;

typedef struct {
    uint8_t mac_address[GAP_MAC_ADDR_SIZE];
    char adv_name[FURI_HAL_BT_ADV_NAME_LENGTH + 1];
    bool bonding_mode;
    GapPairing pairing_method;
} GapConfig;

typedef void* FuriHalBleProfileParams;

struct FuriHalBleProfileTemplate;

typedef struct FuriHalBleProfileBase {
    const struct FuriHalBleProfileTemplate* config;
} FuriHalBleProfileBase;

typedef struct FuriHalBleProfileTemplate {
    FuriHalBleProfileBase* (*start)(FuriHalBleProfileParams profile_params);
    void (*stop)(FuriHalBleProfileBase* profile);
    void (*get_gap_config)(GapConfig* config, FuriHalBleProfileParams profile_params);
} FuriHalBleProfileTemplate;

#ifdef __cplusplus
}
#endif
