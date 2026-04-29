#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_gap_ble_api.h>

#define BLE_UUID_DB_DIR "/ext/ble"
// Files loaded (each optional):
//   /ext/ble/services.txt     — service UUIDs (16-bit + 128-bit)
//   /ext/ble/chars.txt        — characteristic UUIDs
//   /ext/ble/descriptors.txt  — descriptor UUIDs
//   /ext/ble/members.txt      — vendor 16-bit member-allocated UUIDs
// Format per line: "<uuid>  <name>  [optional source-tag]"
//   - uuid: 16-bit hex (180F or 0x180F) or 128-bit (with/without dashes)
//   - tag in [brackets] at end of line is stripped from displayed name
//   - lines starting with # are comments

// Loads any of the four files that exist. Idempotent. Safe to call without SD.
void ble_uuid_db_init(void);

// Frees all loaded maps.
void ble_uuid_db_deinit(void);

// Look up a service UUID name. Returns NULL if unknown.
// Search order: SD services → SD members → built-in SIG service table.
const char* ble_uuid_db_lookup_service(const esp_bt_uuid_t* uuid);

// Look up a characteristic UUID name. Returns NULL if unknown.
// Search order: SD chars → built-in SIG characteristic table.
const char* ble_uuid_db_lookup_char(const esp_bt_uuid_t* uuid);
