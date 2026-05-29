#pragma once

#include <gui/view.h>
#include <lib/flipper_format/flipper_format.h>

typedef struct SubGhzViewTpmsInfo SubGhzViewTpmsInfo;

typedef enum {
    SubGhzViewTpmsFieldPressure = 0,
    SubGhzViewTpmsFieldTemperature,
    SubGhzViewTpmsFieldId,
    SubGhzViewTpmsFieldBattery,
    SubGhzViewTpmsFieldCount,
} SubGhzViewTpmsField;

typedef enum {
    SubGhzViewTpmsInfoActionSave, // Right-button short press
    SubGhzViewTpmsInfoActionTxStart, // OK-button long press (start endless TX)
    SubGhzViewTpmsInfoActionTxStop, // OK-button release after long press
    SubGhzViewTpmsInfoActionEdit, // OK-button short press on a numeric field
    SubGhzViewTpmsInfoActionToggleBattery, // OK-button short press on Battery field
} SubGhzViewTpmsInfoAction;

typedef void (
    *SubGhzViewTpmsInfoActionCallback)(SubGhzViewTpmsInfoAction action, void* context);

SubGhzViewTpmsInfo* subghz_view_tpms_info_alloc(void);

void subghz_view_tpms_info_free(SubGhzViewTpmsInfo* tpms_info);

View* subghz_view_tpms_info_get_view(SubGhzViewTpmsInfo* tpms_info);

/**
 * Push the latest decoded TPMS values into the view model.
 * @param fff           FlipperFormat string-stream serialized by the TPMS decoder.
 * @param frequency     Pre-formatted frequency string ("433.92").
 * @param modulation    Pre-formatted modulation string ("AM650").
 * @param can_send      Whether to draw the "Send" button hint and accept OK input.
 * @param can_save      Whether to draw the "Save" button hint and accept Right input.
 */
void subghz_view_tpms_info_update(
    SubGhzViewTpmsInfo* tpms_info,
    FlipperFormat* fff,
    const char* frequency,
    const char* modulation,
    bool can_send,
    bool can_save);

/**
 * Register a callback that the view fires when the user presses Send / Save / Edit.
 */
void subghz_view_tpms_info_set_callback(
    SubGhzViewTpmsInfo* tpms_info,
    SubGhzViewTpmsInfoActionCallback callback,
    void* context);

/** Currently selected field — used by the Edit scene to know which value to ask for. */
SubGhzViewTpmsField subghz_view_tpms_info_get_selected_field(SubGhzViewTpmsInfo* tpms_info);

/** Push freshly-edited values back into the displayed model (no re-deserialize). */
void subghz_view_tpms_info_set_values(
    SubGhzViewTpmsInfo* tpms_info,
    uint32_t id,
    float pressure,
    float temperature,
    uint8_t battery_low);
