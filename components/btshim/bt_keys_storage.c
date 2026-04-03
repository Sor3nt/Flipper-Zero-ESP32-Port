/**
 * @file bt_keys_storage.c
 * BLE key storage — ESP32 stub
 *
 * ESP32 Bluedroid handles bonding transparently via NVS.
 * This stub keeps the bt_service API surface intact.
 */

#include "bt_keys_storage.h"

#include <furi.h>
#include <ble_hid.h>

#define TAG "BtKeyStorage"

struct BtKeysStorage {
    FuriString* file_path;
    GapRootSecurityKeys root_keys;
};

BtKeysStorage* bt_keys_storage_alloc(const char* keys_storage_path) {
    furi_assert(keys_storage_path);

    BtKeysStorage* instance = malloc(sizeof(BtKeysStorage));
    instance->file_path = furi_string_alloc();
    furi_string_set_str(instance->file_path, keys_storage_path);
    memset(&instance->root_keys, 0, sizeof(instance->root_keys));
    return instance;
}

void bt_keys_storage_free(BtKeysStorage* instance) {
    furi_assert(instance);
    furi_string_free(instance->file_path);
    free(instance);
}

void bt_keys_storage_set_file_path(BtKeysStorage* instance, const char* path) {
    furi_assert(instance);
    furi_assert(path);
    furi_string_set_str(instance->file_path, path);
}

void bt_keys_storage_set_ram_params(BtKeysStorage* instance, uint8_t* buff, uint16_t size) {
    (void)instance;
    (void)buff;
    (void)size;
    /* No-op: ESP32 doesn't use shared SRAM for key storage */
}

bool bt_keys_storage_is_changed(BtKeysStorage* instance) {
    (void)instance;
    /* ESP32 NVS handles persistence — always report unchanged */
    return false;
}

const GapRootSecurityKeys* bt_keys_storage_get_root_keys(BtKeysStorage* instance) {
    furi_assert(instance);
    return &instance->root_keys;
}

bool bt_keys_storage_load(BtKeysStorage* instance) {
    (void)instance;
    /* No-op: Bluedroid loads bonding data from NVS automatically */
    return true;
}

bool bt_keys_storage_update(BtKeysStorage* instance, uint8_t* start_addr, uint32_t size) {
    (void)instance;
    (void)start_addr;
    (void)size;
    /* No-op: Bluedroid saves bonding data to NVS automatically */
    return true;
}

bool bt_keys_storage_delete(BtKeysStorage* instance) {
    (void)instance;

    bool success = ble_hid_remove_pairing();
    FURI_LOG_I(TAG, "Unpair all devices: %s", success ? "ok" : "failed");
    return success;
}
