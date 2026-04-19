/**
 * @file bt_keys_storage.h
 * BLE key storage — ESP32 stub
 *
 * On STM32, pairing keys live in NVM SRAM shared between cores.
 * On ESP32, Bluedroid manages bonding transparently via NVS.
 * This module is kept for bt_service API compatibility.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <gap.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BtKeysStorage BtKeysStorage;

BtKeysStorage* bt_keys_storage_alloc(const char* keys_storage_path);

void bt_keys_storage_free(BtKeysStorage* instance);

void bt_keys_storage_set_file_path(BtKeysStorage* instance, const char* path);

void bt_keys_storage_set_ram_params(BtKeysStorage* instance, uint8_t* buff, uint16_t size);

bool bt_keys_storage_is_changed(BtKeysStorage* instance);

const GapRootSecurityKeys* bt_keys_storage_get_root_keys(BtKeysStorage* instance);

bool bt_keys_storage_load(BtKeysStorage* instance);

bool bt_keys_storage_update(BtKeysStorage* instance, uint8_t* start_addr, uint32_t size);

bool bt_keys_storage_delete(BtKeysStorage* instance);

#ifdef __cplusplus
}
#endif
