#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <lib/flipper_format/flipper_format.h>
#include "furi.h"
#include <furi_hal.h>
#include "../types.h"
#include <locale/locale.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TPMS_NO_BATT 0xFF

#define TPMS_KEY_FILE_VERSION 1
#define TPMS_KEY_FILE_TYPE    "Flipper Tire Pressure Monitoring System Key File"

typedef struct TPMSBlockGeneric TPMSBlockGeneric;

struct TPMSBlockGeneric {
    const char* protocol_name;
    uint64_t data;
    uint8_t data_count_bit;

    uint32_t timestamp;

    uint32_t id;
    uint8_t battery_low;
    float pressure; // bar
    float temperature; // celsius
};

/**
 * Get name preset.
 * @param preset_name name preset
 * @param preset_str Output name preset
 */
void tpms_block_generic_get_preset_name(const char* preset_name, FuriString* preset_str);

/**
 * Serialize data TPMSBlockGeneric.
 */
SubGhzProtocolStatus tpms_block_generic_serialize(
    TPMSBlockGeneric* instance,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

/**
 * Deserialize data TPMSBlockGeneric.
 */
SubGhzProtocolStatus
    tpms_block_generic_deserialize(TPMSBlockGeneric* instance, FlipperFormat* flipper_format);

/**
 * Deserialize data TPMSBlockGeneric with bit-count check.
 */
SubGhzProtocolStatus tpms_block_generic_deserialize_check_count_bit(
    TPMSBlockGeneric* instance,
    FlipperFormat* flipper_format,
    uint16_t count_bit);

#ifdef __cplusplus
}
#endif
