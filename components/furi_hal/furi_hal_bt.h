/**
 * @file furi_hal_bt.h
 * BT/BLE HAL API — ESP32 implementation
 *
 * Mirrors the STM32 furi_hal_bt.h API surface so that bt_service (bt.c)
 * can be used with minimal #ifdefs.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "../furi_ble/profile_interface.h"
#include "../furi_ble/gap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- BLE glue types (STM32 compat) ---- */

typedef struct {
    uint32_t source_pc;
    uint32_t source_lr;
    uint32_t source_sp;
} BleGlueHardfaultInfo;

typedef enum {
    BleGlueC2ModeStack = 0,
    BleGlueC2ModeFUS = 1,
} BleGlueC2Mode;

typedef struct {
    BleGlueC2Mode mode;
    uint8_t StackType;
    uint8_t VersionMajor;
    uint8_t VersionMinor;
    uint8_t VersionSub;
    uint8_t VersionBranch;
    uint8_t VersionReleaseType;
    uint8_t FusVersionMajor;
    uint8_t FusVersionMinor;
    uint8_t FusVersionSub;
    uint8_t MemorySizeSram2B;
    uint8_t MemorySizeSram2A;
    uint8_t FusMemorySizeSram2B;
    uint8_t FusMemorySizeSram2A;
    uint8_t FusMemorySizeFlash;
    const char* StackTypeString;
} BleGlueC2Info;

/* ---- BLE glue functions ---- */

const BleGlueHardfaultInfo* ble_glue_get_hardfault_info(void);
const BleGlueC2Info* ble_glue_get_c2_info(void);

/** Wait for C2 to start (STM32 compat — returns immediately on ESP32) */
static inline bool ble_glue_wait_for_c2_start(uint32_t timeout) {
    (void)timeout;
    return true;
}

/* ---- Radio stack ---- */

/** Start the BLE radio stack.
 *  On ESP32, the Bluedroid stack is initialized lazily when the first
 *  profile is allocated (ble_hid_alloc). This function is kept for
 *  STM32 bt_service compatibility and always returns true. */
bool furi_hal_bt_start_radio_stack(void);

/** Check if GATT/GAP is supported (always true on ESP32) */
bool furi_hal_bt_is_gatt_gap_supported(void);

/** Check if BLE stack is active (profile allocated) */
bool furi_hal_bt_is_active(void);

/* ---- Profile management ---- */

/** Start / change BLE application profile.
 *
 *  On ESP32, this stops the current profile (if any), starts a new one
 *  via the profile template, and installs gap_event_cb as the GAP event
 *  bridge so bt_service receives GapEvents.
 *
 *  @param profile_template  Profile to start
 *  @param params            Profile-specific parameters (may be NULL)
 *  @param root_keys         Ignored on ESP32 (bonding via NVS)
 *  @param event_cb          Callback for GAP events
 *  @param context           Context for event_cb
 *  @return                  Profile instance, or NULL on failure
 */
FuriHalBleProfileBase* furi_hal_bt_change_app(
    const FuriHalBleProfileTemplate* profile_template,
    FuriHalBleProfileParams params,
    const GapRootSecurityKeys* root_keys,
    GapEventCallback event_cb,
    void* context);

/** Check if a profile instance matches a template */
bool furi_hal_bt_check_profile_type(
    FuriHalBleProfileBase* profile,
    const FuriHalBleProfileTemplate* profile_template);

/* ---- Advertising ---- */

void furi_hal_bt_start_advertising(void);
void furi_hal_bt_stop_advertising(void);

/** Reset HAL profile state (call when BLE stack is fully stopped). */
void furi_hal_bt_reinit(void);

/* ---- Battery / power (stubs) ---- */

void furi_hal_bt_update_battery_level(uint8_t battery_level);
void furi_hal_bt_update_power_state(bool charging);

/* ---- Key storage (stubs — ESP32 uses NVS) ---- */

void furi_hal_bt_get_key_storage_buff(uint8_t** key_buff_addr, uint16_t* key_buff_size);
void furi_hal_bt_nvm_sram_sem_acquire(void);
void furi_hal_bt_nvm_sram_sem_release(void);
bool furi_hal_bt_clear_white_list(void);
void furi_hal_bt_set_key_storage_change_callback(
    BleGlueKeyStorageChangedCallback callback,
    void* context);

/* ---- Extra Beacon (non-connectable advertising) ---- */

#include <extra_beacon.h>

bool furi_hal_bt_extra_beacon_is_active(void);
bool furi_hal_bt_extra_beacon_set_config(const GapExtraBeaconConfig* config);
const GapExtraBeaconConfig* furi_hal_bt_extra_beacon_get_config(void);
bool furi_hal_bt_extra_beacon_set_data(const uint8_t* data, uint8_t len);
uint8_t furi_hal_bt_extra_beacon_get_data(uint8_t* data);
bool furi_hal_bt_extra_beacon_start(void);
bool furi_hal_bt_extra_beacon_stop(void);

#ifdef __cplusplus
}
#endif
