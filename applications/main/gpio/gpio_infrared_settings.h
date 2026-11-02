#pragma once

#include "../infrared/infrared_app.h"
#include <furi.h>
#include <toolbox/saved_struct.h>

static inline bool gpio_infrared_settings_load(InfraredSettings* settings) {
    furi_assert(settings);
    return saved_struct_load(
        INFRARED_SETTINGS_PATH,
        settings,
        sizeof(InfraredSettings),
        INFRARED_SETTINGS_MAGIC,
        INFRARED_SETTINGS_VERSION);
}

static inline bool gpio_infrared_settings_save(const InfraredSettings* settings) {
    furi_assert(settings);
    const bool success = saved_struct_save(
        INFRARED_SETTINGS_PATH,
        settings,
        sizeof(InfraredSettings),
        INFRARED_SETTINGS_MAGIC,
        INFRARED_SETTINGS_VERSION);
    if(!success) {
        FURI_LOG_E("GPIO", "Failed to save infrared settings");
    }
    return success;
}
