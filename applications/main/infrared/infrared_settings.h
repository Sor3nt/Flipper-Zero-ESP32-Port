#pragma once

#include <furi_hal_infrared.h>
#include <stdbool.h>

#define INFRARED_SETTINGS_PATH    EXT_PATH("infrared/.infrared.settings")
#define INFRARED_SETTINGS_VERSION (2U)
#define INFRARED_SETTINGS_MAGIC   (0x1FU)

typedef struct {
    FuriHalInfraredTxPin tx_pin;
    bool otg_enabled;
    bool easy_mode;
} InfraredSettings;

bool infrared_settings_load(InfraredSettings* settings);
bool infrared_settings_save(const InfraredSettings* settings);
